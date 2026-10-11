#include "mp4_writer.h"
#include "av_util.h"

#include "../common/log.h"
#include "../common/strings.h"

#include <windows.h>

#include <cstring>

namespace hl::media {

static AVCodecID codec_id_from_name(const std::string &name)
{
	if (name == "h264")
		return AV_CODEC_ID_H264;
	if (name == "hevc")
		return AV_CODEC_ID_HEVC;
	if (name == "av1")
		return AV_CODEC_ID_AV1;
	if (name == "aac")
		return AV_CODEC_ID_AAC;
	if (name == "opus")
		return AV_CODEC_ID_OPUS;
	return AV_CODEC_ID_NONE;
}

Mp4Writer::~Mp4Writer()
{
	abort();
}

bool Mp4Writer::open(const std::string &path, const std::string &format, const std::vector<StreamDesc> &streams,
		     const std::vector<Chapter> &chapters, std::string *err)
{
	path_ = path;
	temp_path_ = path + ".partial";
	bytes_ = 0;
	header_written_ = false;

	int ret = avformat_alloc_output_context2(&ctx_, nullptr, format.c_str(), temp_path_.c_str());
	if (ret < 0 || !ctx_) {
		if (err)
			*err = "Could not create " + format + " muxer: " + av_error(ret);
		return false;
	}

	const bool is_mov = format == "mp4" || format == "mov";

	for (const auto &d : streams) {
		AVStream *st = avformat_new_stream(ctx_, nullptr);
		if (!st) {
			if (err)
				*err = "Out of memory";
			return false;
		}
		AVCodecParameters *par = st->codecpar;
		par->codec_id = codec_id_from_name(d.codec);
		par->bit_rate = d.bitrate;

		if (!d.extradata.empty()) {
			par->extradata = (uint8_t *)av_mallocz(d.extradata.size() + AV_INPUT_BUFFER_PADDING_SIZE);
			memcpy(par->extradata, d.extradata.data(), d.extradata.size());
			par->extradata_size = (int)d.extradata.size();
		}

		if (d.video) {
			par->codec_type = AVMEDIA_TYPE_VIDEO;
			par->width = d.width;
			par->height = d.height;
			par->color_range = AVCOL_RANGE_MPEG;
			if (d.hdr) {
				par->color_primaries = AVCOL_PRI_BT2020;
				par->color_trc = AVCOL_TRC_SMPTE2084;
				par->color_space = AVCOL_SPC_BT2020_NCL;
			} else {
				par->color_primaries = AVCOL_PRI_BT709;
				par->color_trc = AVCOL_TRC_BT709;
				par->color_space = AVCOL_SPC_BT709;
			}
			par->chroma_location = AVCHROMA_LOC_LEFT;
			if (is_mov && par->codec_id == AV_CODEC_ID_HEVC)
				par->codec_tag = MKTAG('h', 'v', 'c', '1');
			st->time_base = AVRational{d.fps_den, d.fps_num};
			st->avg_frame_rate = AVRational{d.fps_num, d.fps_den};
		} else {
			par->codec_type = AVMEDIA_TYPE_AUDIO;
			par->sample_rate = d.sample_rate;
			par->frame_size = d.frame_size;
			av_channel_layout_default(&par->ch_layout, d.channels);
			st->time_base = AVRational{1, d.sample_rate};
		}

		if (!d.title.empty()) {
			av_dict_set(&st->metadata, "title", d.title.c_str(), 0);
			av_dict_set(&st->metadata, "handler_name", d.title.c_str(), 0);
		}
	}

	if (!chapters.empty()) {
		ctx_->chapters = (AVChapter **)av_calloc(chapters.size(), sizeof(AVChapter *));
		for (size_t i = 0; i < chapters.size(); i++) {
			AVChapter *ch = (AVChapter *)av_mallocz(sizeof(AVChapter));
			ch->id = (int64_t)i + 1;
			ch->time_base = AVRational{1, 1000};
			ch->start = (int64_t)(chapters[i].start * 1000.0);
			ch->end = (int64_t)(chapters[i].end * 1000.0);
			av_dict_set(&ch->metadata, "title", chapters[i].title.c_str(), 0);
			ctx_->chapters[i] = ch;
			ctx_->nb_chapters = (unsigned)(i + 1);
		}
	}

	av_dict_set(&ctx_->metadata, "encoder", "Highlight", 0);

	if (!(ctx_->oformat->flags & AVFMT_NOFILE)) {
		ret = avio_open(&ctx_->pb, temp_path_.c_str(), AVIO_FLAG_WRITE);
		if (ret < 0) {
			if (err)
				*err = "Could not create " + temp_path_ + ": " + av_error(ret);
			return false;
		}
	}

	AVDictionary *opts = nullptr;
	if (is_mov)
		av_dict_set(&opts, "movflags", "+faststart", 0);
	ret = avformat_write_header(ctx_, &opts);
	av_dict_free(&opts);
	if (ret < 0) {
		if (err)
			*err = "Could not write file header: " + av_error(ret);
		return false;
	}
	header_written_ = true;
	last_dts_.assign(streams.size(), INT64_MIN);
	return true;
}

bool Mp4Writer::write(int stream, const uint8_t *data, size_t size, int64_t pts, int64_t dts, int32_t timebase_den,
		      bool keyframe)
{
	if (!ctx_ || !header_written_ || stream < 0 || stream >= (int)ctx_->nb_streams)
		return false;

	AVStream *st = ctx_->streams[stream];
	AVRational src{1, timebase_den};

	PacketPtr pkt = make_packet();
	pkt->data = const_cast<uint8_t *>(data);
	pkt->size = (int)size;
	pkt->stream_index = stream;
	pkt->pts = av_rescale_q_rnd(pts, src, st->time_base, (AVRounding)(AV_ROUND_NEAR_INF | AV_ROUND_PASS_MINMAX));
	pkt->dts = av_rescale_q_rnd(dts, src, st->time_base, (AVRounding)(AV_ROUND_NEAR_INF | AV_ROUND_PASS_MINMAX));
	if (keyframe)
		pkt->flags |= AV_PKT_FLAG_KEY;

	/* Muxers reject non-increasing DTS; rounding can produce duplicates. */
	int64_t &last = last_dts_[stream];
	if (last != INT64_MIN && pkt->dts <= last) {
		pkt->dts = last + 1;
		if (pkt->pts < pkt->dts)
			pkt->pts = pkt->dts;
	}
	last = pkt->dts;

	int ret = av_interleaved_write_frame(ctx_, pkt.get());
	if (ret < 0) {
		HL_WARN("av_interleaved_write_frame failed: %s", av_error(ret).c_str());
		return ret == AVERROR_INVALIDDATA || ret == AVERROR(EINVAL);
	}
	bytes_ += size;
	return true;
}

bool Mp4Writer::finish(std::string *err)
{
	if (!ctx_)
		return false;

	int ret = header_written_ ? av_write_trailer(ctx_) : -1;
	if (!(ctx_->oformat->flags & AVFMT_NOFILE))
		avio_closep(&ctx_->pb);
	avformat_free_context(ctx_);
	ctx_ = nullptr;

	if (ret < 0) {
		if (err)
			*err = "Could not finish the file: " + av_error(ret);
		DeleteFileW(widen(temp_path_).c_str());
		return false;
	}

	if (!MoveFileExW(widen(temp_path_).c_str(), widen(path_).c_str(), MOVEFILE_REPLACE_EXISTING)) {
		if (err)
			*err = "Could not rename the finished file (error " + std::to_string(GetLastError()) + ")";
		return false;
	}
	return true;
}

void Mp4Writer::abort()
{
	if (!ctx_)
		return;
	if (!(ctx_->oformat->flags & AVFMT_NOFILE))
		avio_closep(&ctx_->pb);
	avformat_free_context(ctx_);
	ctx_ = nullptr;
	DeleteFileW(widen(temp_path_).c_str());
}

} // namespace hl::media

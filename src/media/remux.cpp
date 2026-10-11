#include "remux.h"
#include "av_util.h"

#include "../common/strings.h"

#include <windows.h>

#include <algorithm>
#include <cmath>

namespace hl::media {

bool remux(const std::string &in, const std::string &out, const std::string &format, std::string *err)
{
	TrimOptions opt;
	opt.format = format;
	return trim_lossless(in, out, opt, err);
}

bool trim_lossless(const std::string &in_path, const std::string &out_path, const TrimOptions &opt, std::string *err,
		   double *actual_start)
{
	InputPtr in = open_input(in_path, err);
	if (!in)
		return false;
	AVFormatContext *ic = in.get();

	const std::string tmp = out_path + ".partial";
	AVFormatContext *oc = nullptr;
	int ret = avformat_alloc_output_context2(&oc, nullptr, opt.format.c_str(), tmp.c_str());
	if (ret < 0 || !oc) {
		if (err)
			*err = "Could not create the output: " + av_error(ret);
		return false;
	}
	auto cleanup = [&](bool remove) {
		if (oc) {
			if (!(oc->oformat->flags & AVFMT_NOFILE))
				avio_closep(&oc->pb);
			avformat_free_context(oc);
			oc = nullptr;
		}
		if (remove)
			DeleteFileW(widen(tmp).c_str());
	};

	const bool is_mov = opt.format == "mp4" || opt.format == "mov";
	std::vector<int> map(ic->nb_streams, -1);
	int video_in = -1;

	for (unsigned i = 0; i < ic->nb_streams; i++) {
		AVStream *ist = ic->streams[i];
		AVMediaType type = ist->codecpar->codec_type;
		if (type == AVMEDIA_TYPE_VIDEO) {
			if (video_in >= 0 || (ist->disposition & AV_DISPOSITION_ATTACHED_PIC))
				continue;
			video_in = (int)i;
		} else if (type == AVMEDIA_TYPE_AUDIO) {
			if (!opt.audio_streams.empty() &&
			    std::find(opt.audio_streams.begin(), opt.audio_streams.end(), (int)i) == opt.audio_streams.end())
				continue;
		} else {
			continue;
		}

		AVStream *ost = avformat_new_stream(oc, nullptr);
		avcodec_parameters_copy(ost->codecpar, ist->codecpar);
		ost->codecpar->codec_tag = 0;
		if (is_mov && ost->codecpar->codec_id == AV_CODEC_ID_HEVC)
			ost->codecpar->codec_tag = MKTAG('h', 'v', 'c', '1');
		ost->time_base = ist->time_base;
		ost->avg_frame_rate = ist->avg_frame_rate;
		ost->disposition = ist->disposition;
		av_dict_copy(&ost->metadata, ist->metadata, 0);
		map[i] = ost->index;
	}

	if (video_in < 0) {
		if (err)
			*err = "The file has no video";
		cleanup(true);
		return false;
	}

	const double end = opt.end > 0 ? opt.end : 1e18;

	/* Seek to the keyframe at or before start. */
	if (opt.start > 0.01) {
		int64_t ts = (int64_t)(opt.start * AV_TIME_BASE);
		av_seek_frame(ic, -1, ts, AVSEEK_FLAG_BACKWARD);
	}

	if (!(oc->oformat->flags & AVFMT_NOFILE)) {
		ret = avio_open(&oc->pb, tmp.c_str(), AVIO_FLAG_WRITE);
		if (ret < 0) {
			if (err)
				*err = "Could not create " + out_path + ": " + av_error(ret);
			cleanup(true);
			return false;
		}
	}

	PacketPtr pkt = make_packet();
	std::vector<PacketPtr> held; /* packets read before the header is written */
	double t0 = -1;              /* time of the first kept video keyframe */

	/* The backward seek lands on a keyframe at or before start. Read up to that
	 * keyframe (holding audio that may belong after it) so chapters can be shifted
	 * before the header is written. */
	while ((ret = av_read_frame(ic, pkt.get())) >= 0) {
		int si = pkt->stream_index;
		if (map[si] < 0) {
			av_packet_unref(pkt.get());
			continue;
		}
		if (si == video_in) {
			if (!(pkt->flags & AV_PKT_FLAG_KEY) || pkt->pts == AV_NOPTS_VALUE) {
				av_packet_unref(pkt.get());
				continue;
			}
			t0 = (double)pkt->pts * av_q2d(ic->streams[si]->time_base);
			PacketPtr copy = make_packet();
			av_packet_move_ref(copy.get(), pkt.get());
			held.push_back(std::move(copy));
			break;
		}
		PacketPtr copy = make_packet();
		av_packet_move_ref(copy.get(), pkt.get());
		held.push_back(std::move(copy));
	}
	if (t0 < 0) {
		if (err)
			*err = "No keyframe found in the selected range";
		cleanup(true);
		return false;
	}
	if (actual_start)
		*actual_start = t0;

	/* Chapters inside the range, shifted. */
	std::vector<AVChapter *> chapters;
	for (unsigned i = 0; i < ic->nb_chapters; i++) {
		AVChapter *c = ic->chapters[i];
		double cs = (double)c->start * av_q2d(c->time_base);
		double ce = (double)c->end * av_q2d(c->time_base);
		if (ce <= t0 || cs >= end)
			continue;
		AVChapter *n = (AVChapter *)av_mallocz(sizeof(AVChapter));
		n->id = (int64_t)chapters.size() + 1;
		n->time_base = AVRational{1, 1000};
		n->start = (int64_t)(std::max(0.0, cs - t0) * 1000);
		n->end = (int64_t)((std::min(ce, end) - t0) * 1000);
		av_dict_copy(&n->metadata, c->metadata, 0);
		chapters.push_back(n);
	}
	if (!chapters.empty()) {
		oc->chapters = (AVChapter **)av_calloc(chapters.size(), sizeof(AVChapter *));
		for (size_t i = 0; i < chapters.size(); i++)
			oc->chapters[i] = chapters[i];
		oc->nb_chapters = (unsigned)chapters.size();
	}

	AVDictionary *opts = nullptr;
	if (is_mov)
		av_dict_set(&opts, "movflags", "+faststart", 0);
	ret = avformat_write_header(oc, &opts);
	av_dict_free(&opts);
	if (ret < 0) {
		if (err)
			*err = "Could not write the header: " + av_error(ret);
		cleanup(true);
		return false;
	}

	std::vector<int64_t> last_dts(oc->nb_streams, INT64_MIN);
	bool video_done = false;

	auto write = [&](AVPacket *p) -> bool {
		int si = p->stream_index;
		AVStream *ist = ic->streams[si];
		AVStream *ost = oc->streams[map[si]];
		double t = (p->pts != AV_NOPTS_VALUE ? p->pts : p->dts) * av_q2d(ist->time_base);
		if (t < t0 - 0.0005 && ist->codecpar->codec_type == AVMEDIA_TYPE_AUDIO)
			return true; /* audio before the cut */
		if (t > end) {
			if (si == video_in)
				video_done = true;
			return true;
		}
		int64_t off = av_rescale_q((int64_t)llround(t0 * 1000000), AVRational{1, 1000000}, ist->time_base);
		if (p->pts != AV_NOPTS_VALUE)
			p->pts -= off;
		if (p->dts != AV_NOPTS_VALUE)
			p->dts -= off;
		av_packet_rescale_ts(p, ist->time_base, ost->time_base);
		p->stream_index = ost->index;
		p->pos = -1;
		int64_t &ld = last_dts[ost->index];
		if (p->dts != AV_NOPTS_VALUE) {
			if (ld != INT64_MIN && p->dts <= ld)
				p->dts = ld + 1;
			if (p->pts != AV_NOPTS_VALUE && p->pts < p->dts)
				p->pts = p->dts;
			ld = p->dts;
		}
		int r = av_interleaved_write_frame(oc, p);
		return r >= 0 || r == AVERROR_INVALIDDATA;
	};

	bool ok = true;
	for (auto &h : held)
		if (!write(h.get())) {
			ok = false;
			break;
		}
	held.clear();

	while (ok && !video_done && (ret = av_read_frame(ic, pkt.get())) >= 0) {
		if (map[pkt->stream_index] >= 0)
			ok = write(pkt.get());
		av_packet_unref(pkt.get());
	}

	if (!ok) {
		if (err)
			*err = "Could not write the trimmed file";
		cleanup(true);
		return false;
	}

	av_write_trailer(oc);
	cleanup(false);
	in.reset();

	if (!MoveFileExW(widen(tmp).c_str(), widen(out_path).c_str(), MOVEFILE_REPLACE_EXISTING)) {
		if (err)
			*err = "Could not rename the output file";
		DeleteFileW(widen(tmp).c_str());
		return false;
	}
	return true;
}

} // namespace hl::media

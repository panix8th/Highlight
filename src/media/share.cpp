#include "share.h"
#include "av_util.h"
#include "remux.h"

#include "../common/log.h"
#include "../common/strings.h"

#include <windows.h>
#include <shlobj.h>

#include <algorithm>
#include <cmath>
#include <filesystem>

namespace hl::media {

namespace {

struct CodecInfo {
	const char *name;
	double threshold_bpp; /* below this many bits per pixel the picture gets visibly soft */
	double best_bpp;      /* typical rate of a visually lossless constant-quality encode */
	int cq;
	const char *nvenc;
	const char *fallback;
};

const CodecInfo k_codecs[] = {
	{"h264", 0.060, 0.090, 21, "h264_nvenc", "libx264"},
	{"hevc", 0.042, 0.060, 24, "hevc_nvenc", "libx264"},
	{"av1", 0.035, 0.050, 32, "av1_nvenc", "libsvtav1"},
};

const CodecInfo &codec_info(const std::string &name)
{
	for (const auto &c : k_codecs)
		if (name == c.name)
			return c;
	return k_codecs[0];
}

} // namespace

/* ------------------------------------------------------------------ */
/* planning                                                           */

SharePlan plan_share(const MediaInfo &info, const ShareRequest &req)
{
	SharePlan p;
	const CodecInfo &c = codec_info(req.codec);
	p.codec = c.name;
	p.cq = c.cq;
	p.target_bytes = req.target_bytes;

	double start = std::max(0.0, req.start);
	double end = req.end > 0 ? std::min(req.end, info.duration) : info.duration;
	p.duration = std::max(0.1, end - start);

	const int src_w = std::max(2, info.width);
	const int src_h = std::max(2, info.height);
	int src_fps = (int)std::lround(info.fps);
	if (src_fps <= 0)
		src_fps = 60;
	src_fps = std::clamp(src_fps, 1, 240);
	const int hi_fps = std::min(src_fps, 60);

	const bool has_audio = !info.audio.empty();
	if (!has_audio)
		p.audio_bitrate = 0;
	else if (req.target_bytes > 0 && req.target_bytes <= 12ll * 1024 * 1024)
		p.audio_bitrate = 96000;
	else if (req.target_bytes > 0 && req.target_bytes <= 60ll * 1024 * 1024)
		p.audio_bitrate = 128000;
	else
		p.audio_bitrate = 160000;

	auto width_for = [&](int h) { return (int)std::lround((double)src_w * h / src_h) & ~1; };

	/* Best quality: source resolution and frame rate, constant quality. */
	double best_bps = (double)src_w * src_h * src_fps * c.best_bpp;
	if (info.video_bitrate > 0)
		best_bps = std::min(best_bps, (double)info.video_bitrate * 0.9);
	int64_t best_bytes = (int64_t)((best_bps + p.audio_bitrate) * p.duration / 8.0);

	if (req.target_bytes <= 0 || best_bytes <= (int64_t)(req.target_bytes * 0.92)) {
		p.width = src_w & ~1;
		p.height = src_h & ~1;
		p.fps = src_fps;
		p.video_bitrate = 0;
		p.estimated_bytes = best_bytes;
		p.quality = "Identical";
		p.quality_level = 4;
		return p;
	}

	/* Fit a size limit. */
	const double total_bits = (double)req.target_bytes * 8.0 * 0.96;
	double video_bps = total_bits / p.duration - p.audio_bitrate;
	video_bps = std::max(video_bps, 100000.0);

	struct Cand {
		int h, fps;
		double rate;
	};
	std::vector<Cand> cands;
	const int heights[] = {2160, 1440, 1080, 900, 720, 540, 480, 360};
	bool have_src = false;
	for (int h : heights)
		have_src |= h == src_h;
	if (!have_src)
		cands.push_back({src_h & ~1, hi_fps, (double)src_w * src_h * hi_fps});
	for (int h : heights) {
		if (h > src_h)
			continue;
		double px = (double)width_for(h) * h;
		cands.push_back({h, hi_fps, px * hi_fps});
		if (hi_fps > 30)
			cands.push_back({h, 30, px * 30});
	}
	std::sort(cands.begin(), cands.end(), [](const Cand &a, const Cand &b) { return a.rate > b.rate; });

	const Cand *pick = nullptr;
	double ratio = 0;
	for (const auto &cand : cands) {
		double bpp = video_bps / cand.rate;
		if (bpp >= c.threshold_bpp) {
			pick = &cand;
			ratio = bpp / c.threshold_bpp;
			break;
		}
	}
	if (!pick && !cands.empty()) {
		pick = &cands.back();
		ratio = video_bps / pick->rate / c.threshold_bpp;
	}

	p.height = pick ? pick->h : (src_h & ~1);
	p.width = width_for(p.height);
	p.fps = pick ? pick->fps : hi_fps;
	p.video_bitrate = (int64_t)video_bps;
	p.estimated_bytes = (int64_t)(req.target_bytes * 0.95);

	if (ratio >= 2.0) {
		p.quality = "Identical";
		p.quality_level = 4;
	} else if (ratio >= 1.35) {
		p.quality = "Excellent";
		p.quality_level = 3;
	} else if (ratio >= 1.0) {
		p.quality = "Great";
		p.quality_level = 2;
	} else {
		p.quality = "Soft";
		p.quality_level = 1;
	}
	return p;
}

/* ------------------------------------------------------------------ */
/* encoding                                                           */

namespace {

class ShareJob {
public:
	ShareJob(const ShareRequest &req, const SharePlan &plan) : req_(req), plan_(plan) {}
	~ShareJob() { close_output(true); }

	bool run(const std::function<bool(float)> &progress, std::string *err, int64_t *bytes);

private:
	bool open_decoders();
	bool open_encoders();
	bool open_output();
	bool init_audio_graph();
	bool init_video_graph(const AVFrame *first);

	bool decode_video(AVPacket *pkt);
	bool handle_video_frame(AVFrame *f);
	bool pull_video(bool flush);
	bool decode_audio(size_t idx, AVPacket *pkt);
	bool handle_audio_frame(size_t idx, AVFrame *f);
	bool pull_audio();
	bool encode(AVCodecContext *enc, AVStream *st, AVFrame *f);
	void close_output(bool remove);

	ShareRequest req_;
	SharePlan plan_;
	std::string err_;

	InputPtr in_;
	int vidx_ = -1;
	CodecPtr vdec_;
	std::vector<int> aidx_;
	std::vector<CodecPtr> adecs_;

	CodecPtr venc_;
	CodecPtr aenc_;
	AVFormatContext *oc_ = nullptr;
	AVStream *vst_ = nullptr;
	AVStream *ast_ = nullptr;
	std::string tmp_path_;

	GraphPtr vgraph_;
	AVFilterContext *vsrc_ = nullptr;
	AVFilterContext *vsink_ = nullptr;
	GraphPtr agraph_;
	std::vector<AVFilterContext *> asrcs_;
	AVFilterContext *asink_ = nullptr;

	double start_ = 0;
	double end_ = 1e18;
	bool video_eof_ = false;
	std::vector<bool> audio_eof_;
	double last_video_t_ = 0;
	std::string out_codec_;
};

bool ShareJob::open_decoders()
{
	in_ = open_input(req_.input, &err_);
	if (!in_)
		return false;
	AVFormatContext *ic = in_.get();

	vidx_ = av_find_best_stream(ic, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
	if (vidx_ < 0) {
		err_ = "The clip has no video";
		return false;
	}
	AVStream *vst = ic->streams[vidx_];
	AVCodecParameters *par = vst->codecpar;

	/* NVDEC first: fast and keeps the CPU free. */
	const char *hw = par->codec_id == AV_CODEC_ID_HEVC   ? "hevc_cuvid"
			 : par->codec_id == AV_CODEC_ID_H264 ? "h264_cuvid"
			 : par->codec_id == AV_CODEC_ID_AV1  ? "av1_cuvid"
							     : nullptr;
	if (hw) {
		if (const AVCodec *dec = avcodec_find_decoder_by_name(hw)) {
			CodecPtr ctx(avcodec_alloc_context3(dec));
			avcodec_parameters_to_context(ctx.get(), par);
			ctx->pkt_timebase = vst->time_base;
			if (avcodec_open2(ctx.get(), dec, nullptr) >= 0)
				vdec_ = std::move(ctx);
		}
	}
	if (!vdec_) {
		const AVCodec *dec = avcodec_find_decoder(par->codec_id);
		if (!dec) {
			err_ = "No decoder for this video";
			return false;
		}
		CodecPtr ctx(avcodec_alloc_context3(dec));
		avcodec_parameters_to_context(ctx.get(), par);
		ctx->pkt_timebase = vst->time_base;
		ctx->thread_count = 0;
		int ret = avcodec_open2(ctx.get(), dec, nullptr);
		if (ret < 0) {
			err_ = "Could not open the video decoder: " + av_error(ret);
			return false;
		}
		vdec_ = std::move(ctx);
	}
	HL_INFO("Share: decoding with %s", vdec_->codec->name);

	std::vector<int> wanted = req_.audio_streams;
	if (wanted.empty()) {
		int a = av_find_best_stream(ic, AVMEDIA_TYPE_AUDIO, -1, -1, nullptr, 0);
		if (a >= 0)
			wanted.push_back(a);
	}
	if (plan_.audio_bitrate <= 0)
		wanted.clear();

	for (int idx : wanted) {
		if (idx < 0 || idx >= (int)ic->nb_streams || ic->streams[idx]->codecpar->codec_type != AVMEDIA_TYPE_AUDIO)
			continue;
		AVStream *st = ic->streams[idx];
		const AVCodec *dec = avcodec_find_decoder(st->codecpar->codec_id);
		if (!dec)
			continue;
		CodecPtr ctx(avcodec_alloc_context3(dec));
		avcodec_parameters_to_context(ctx.get(), st->codecpar);
		ctx->pkt_timebase = st->time_base;
		if (avcodec_open2(ctx.get(), dec, nullptr) < 0)
			continue;
		if (ctx->ch_layout.order == AV_CHANNEL_ORDER_UNSPEC) {
			int ch = ctx->ch_layout.nb_channels > 0 ? ctx->ch_layout.nb_channels : 2;
			av_channel_layout_uninit(&ctx->ch_layout);
			av_channel_layout_default(&ctx->ch_layout, ch);
		}
		aidx_.push_back(idx);
		adecs_.push_back(std::move(ctx));
	}
	audio_eof_.assign(aidx_.size(), false);
	return true;
}

static void set_opt(void *obj, const char *key, const char *val)
{
	av_opt_set(obj, key, val, 0); /* options missing on older encoders are fine to skip */
}

bool ShareJob::open_encoders()
{
	const CodecInfo &c = codec_info(plan_.codec);
	const bool target = plan_.video_bitrate > 0;

	auto try_open = [&](const char *name) -> bool {
		const AVCodec *enc = avcodec_find_encoder_by_name(name);
		if (!enc)
			return false;
		CodecPtr ctx(avcodec_alloc_context3(enc));
		ctx->width = plan_.width;
		ctx->height = plan_.height;
		const bool svt = strcmp(name, "libsvtav1") == 0;
		ctx->pix_fmt = svt ? AV_PIX_FMT_YUV420P : AV_PIX_FMT_NV12;
		ctx->time_base = AVRational{1, plan_.fps};
		ctx->framerate = AVRational{plan_.fps, 1};
		ctx->gop_size = plan_.fps * 2;
		ctx->sample_aspect_ratio = AVRational{1, 1};
		ctx->color_range = AVCOL_RANGE_MPEG;
		ctx->color_primaries = AVCOL_PRI_BT709;
		ctx->color_trc = AVCOL_TRC_BT709;
		ctx->colorspace = AVCOL_SPC_BT709;
		ctx->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;

		const bool nvenc = strstr(name, "_nvenc") != nullptr;
		if (nvenc) {
			ctx->max_b_frames = strcmp(name, "av1_nvenc") == 0 ? 0 : 3;
			set_opt(ctx->priv_data, "preset", "p7");
			set_opt(ctx->priv_data, "tune", "hq");
			set_opt(ctx->priv_data, "multipass", "fullres");
			set_opt(ctx->priv_data, "rc", "vbr");
			set_opt(ctx->priv_data, "spatial-aq", "1");
			set_opt(ctx->priv_data, "temporal-aq", "1");
			set_opt(ctx->priv_data, "rc-lookahead", "32");
			if (target) {
				ctx->bit_rate = plan_.video_bitrate;
				ctx->rc_max_rate = plan_.video_bitrate * 2;
				ctx->rc_buffer_size = (int)std::min<int64_t>(plan_.video_bitrate * 2, INT32_MAX);
			} else {
				ctx->bit_rate = 0;
				av_opt_set_int(ctx->priv_data, "cq", plan_.cq, 0);
			}
		} else if (svt) {
			set_opt(ctx->priv_data, "preset", "8");
			if (target)
				ctx->bit_rate = plan_.video_bitrate;
			else
				set_opt(ctx->priv_data, "crf", "32");
		} else {
			ctx->max_b_frames = 3;
			set_opt(ctx->priv_data, "preset", "slow");
			if (target) {
				ctx->bit_rate = plan_.video_bitrate;
				ctx->rc_max_rate = plan_.video_bitrate * 3 / 2;
				ctx->rc_buffer_size = (int)std::min<int64_t>(plan_.video_bitrate * 2, INT32_MAX);
			} else {
				set_opt(ctx->priv_data, "crf", "20");
			}
		}

		int ret = avcodec_open2(ctx.get(), enc, nullptr);
		if (ret < 0) {
			HL_WARN("Share: %s unavailable (%s)", name, av_error(ret).c_str());
			return false;
		}
		venc_ = std::move(ctx);
		const AVCodecDescriptor *d = avcodec_descriptor_get(enc->id);
		out_codec_ = d ? d->name : name;
		HL_INFO("Share: encoding with %s", name);
		return true;
	};

	if (!try_open(c.nvenc) && !try_open(c.fallback) && !try_open("libx264")) {
		err_ = "No video encoder is available for the share copy";
		return false;
	}

	if (!adecs_.empty()) {
		const AVCodec *aenc = avcodec_find_encoder(AV_CODEC_ID_AAC);
		if (aenc) {
			CodecPtr ctx(avcodec_alloc_context3(aenc));
			ctx->sample_fmt = AV_SAMPLE_FMT_FLTP;
			ctx->sample_rate = 48000;
			AVChannelLayout stereo = AV_CHANNEL_LAYOUT_STEREO;
			av_channel_layout_copy(&ctx->ch_layout, &stereo);
			ctx->bit_rate = plan_.audio_bitrate;
			ctx->time_base = AVRational{1, 48000};
			ctx->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
			if (avcodec_open2(ctx.get(), aenc, nullptr) >= 0)
				aenc_ = std::move(ctx);
		}
		if (!aenc_) {
			HL_WARN("Share: AAC encoder unavailable, exporting without audio");
			adecs_.clear();
			aidx_.clear();
			audio_eof_.clear();
		}
	}
	return true;
}

bool ShareJob::open_output()
{
	tmp_path_ = req_.output + ".partial";
	int ret = avformat_alloc_output_context2(&oc_, nullptr, "mp4", tmp_path_.c_str());
	if (ret < 0 || !oc_) {
		err_ = "Could not create the MP4 file";
		return false;
	}

	vst_ = avformat_new_stream(oc_, nullptr);
	avcodec_parameters_from_context(vst_->codecpar, venc_.get());
	vst_->time_base = venc_->time_base;
	vst_->avg_frame_rate = venc_->framerate;
	if (venc_->codec_id == AV_CODEC_ID_HEVC)
		vst_->codecpar->codec_tag = MKTAG('h', 'v', 'c', '1');

	if (aenc_) {
		ast_ = avformat_new_stream(oc_, nullptr);
		avcodec_parameters_from_context(ast_->codecpar, aenc_.get());
		ast_->time_base = aenc_->time_base;
	}

	ret = avio_open(&oc_->pb, tmp_path_.c_str(), AVIO_FLAG_WRITE);
	if (ret < 0) {
		err_ = "Could not create " + req_.output + ": " + av_error(ret);
		return false;
	}
	AVDictionary *opts = nullptr;
	av_dict_set(&opts, "movflags", "+faststart", 0);
	av_dict_set(&oc_->metadata, "encoder", "Highlight", 0);
	ret = avformat_write_header(oc_, &opts);
	av_dict_free(&opts);
	if (ret < 0) {
		err_ = "Could not write the MP4 header: " + av_error(ret);
		return false;
	}
	return true;
}

void ShareJob::close_output(bool remove)
{
	if (oc_) {
		avio_closep(&oc_->pb);
		avformat_free_context(oc_);
		oc_ = nullptr;
		if (remove && !tmp_path_.empty())
			DeleteFileW(widen(tmp_path_).c_str());
	}
}

bool ShareJob::init_audio_graph()
{
	if (adecs_.empty())
		return true;
	agraph_.reset(avfilter_graph_alloc());

	AVFilterInOut *outputs = nullptr;
	std::string desc;
	for (size_t i = 0; i < adecs_.size(); i++) {
		AVCodecContext *d = adecs_[i].get();
		char layout[128] = {};
		av_channel_layout_describe(&d->ch_layout, layout, sizeof(layout));
		AVRational tb = in_->streams[aidx_[i]]->time_base;
		char args[512];
		snprintf(args, sizeof(args), "time_base=%d/%d:sample_rate=%d:sample_fmt=%s:channel_layout=%s", tb.num,
			 tb.den, d->sample_rate, av_get_sample_fmt_name(d->sample_fmt), layout);
		AVFilterContext *src = nullptr;
		std::string name = "in" + std::to_string(i);
		int ret = avfilter_graph_create_filter(&src, avfilter_get_by_name("abuffer"), name.c_str(), args, nullptr,
						       agraph_.get());
		if (ret < 0) {
			err_ = "Audio filter setup failed: " + av_error(ret);
			avfilter_inout_free(&outputs);
			return false;
		}
		asrcs_.push_back(src);

		AVFilterInOut *io = avfilter_inout_alloc();
		io->name = av_strdup(name.c_str());
		io->filter_ctx = src;
		io->pad_idx = 0;
		io->next = outputs;
		outputs = io;
		desc += "[" + name + "]";
	}

	int ret = avfilter_graph_create_filter(&asink_, avfilter_get_by_name("abuffersink"), "out", nullptr, nullptr,
					       agraph_.get());
	if (ret < 0) {
		err_ = "Audio sink setup failed";
		avfilter_inout_free(&outputs);
		return false;
	}
	AVFilterInOut *inputs = avfilter_inout_alloc();
	inputs->name = av_strdup("out");
	inputs->filter_ctx = asink_;
	inputs->pad_idx = 0;
	inputs->next = nullptr;

	if (adecs_.size() > 1)
		desc += "amix=inputs=" + std::to_string(adecs_.size()) + ":normalize=0:duration=longest,";
	desc += "aformat=sample_fmts=fltp:sample_rates=48000:channel_layouts=stereo[out]";

	ret = avfilter_graph_parse_ptr(agraph_.get(), desc.c_str(), &inputs, &outputs, nullptr);
	avfilter_inout_free(&inputs);
	avfilter_inout_free(&outputs);
	if (ret < 0 || (ret = avfilter_graph_config(agraph_.get(), nullptr)) < 0) {
		err_ = "Audio filter graph failed: " + av_error(ret);
		return false;
	}
	av_buffersink_set_frame_size(asink_, (unsigned)aenc_->frame_size);
	return true;
}

bool ShareJob::init_video_graph(const AVFrame *first)
{
	vgraph_.reset(avfilter_graph_alloc());
	AVRational tb = in_->streams[vidx_]->time_base;
	char args[512];
	snprintf(args, sizeof(args), "video_size=%dx%d:pix_fmt=%d:time_base=%d/%d:pixel_aspect=1/1", first->width,
		 first->height, first->format, tb.num, tb.den);
	int ret = avfilter_graph_create_filter(&vsrc_, avfilter_get_by_name("buffer"), "in", args, nullptr,
					       vgraph_.get());
	if (ret >= 0)
		ret = avfilter_graph_create_filter(&vsink_, avfilter_get_by_name("buffersink"), "out", nullptr, nullptr,
						   vgraph_.get());
	if (ret < 0) {
		err_ = "Video filter setup failed: " + av_error(ret);
		return false;
	}

	char desc[256];
	snprintf(desc, sizeof(desc), "scale=%d:%d:flags=lanczos,fps=%d,format=%s", plan_.width, plan_.height,
		 plan_.fps, av_get_pix_fmt_name(venc_->pix_fmt));

	AVFilterInOut *outputs = avfilter_inout_alloc();
	AVFilterInOut *inputs = avfilter_inout_alloc();
	outputs->name = av_strdup("in");
	outputs->filter_ctx = vsrc_;
	outputs->pad_idx = 0;
	outputs->next = nullptr;
	inputs->name = av_strdup("out");
	inputs->filter_ctx = vsink_;
	inputs->pad_idx = 0;
	inputs->next = nullptr;
	ret = avfilter_graph_parse_ptr(vgraph_.get(), desc, &inputs, &outputs, nullptr);
	avfilter_inout_free(&inputs);
	avfilter_inout_free(&outputs);
	if (ret < 0 || (ret = avfilter_graph_config(vgraph_.get(), nullptr)) < 0) {
		err_ = "Video filter graph failed: " + av_error(ret);
		return false;
	}
	return true;
}

bool ShareJob::encode(AVCodecContext *enc, AVStream *st, AVFrame *f)
{
	int ret = avcodec_send_frame(enc, f);
	if (ret < 0 && ret != AVERROR_EOF) {
		err_ = "Encoding failed: " + av_error(ret);
		return false;
	}
	PacketPtr pkt = make_packet();
	for (;;) {
		ret = avcodec_receive_packet(enc, pkt.get());
		if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF)
			return true;
		if (ret < 0) {
			err_ = "Encoding failed: " + av_error(ret);
			return false;
		}
		av_packet_rescale_ts(pkt.get(), enc->time_base, st->time_base);
		pkt->stream_index = st->index;
		ret = av_interleaved_write_frame(oc_, pkt.get());
		if (ret < 0) {
			err_ = "Could not write the share copy: " + av_error(ret);
			return false;
		}
	}
}

bool ShareJob::pull_video(bool)
{
	FramePtr f = make_frame();
	AVRational sink_tb = av_buffersink_get_time_base(vsink_);
	for (;;) {
		int ret = av_buffersink_get_frame(vsink_, f.get());
		if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF)
			return true;
		if (ret < 0) {
			err_ = "Video filter failed";
			return false;
		}
		if (f->pts != AV_NOPTS_VALUE)
			f->pts = av_rescale_q(f->pts, sink_tb, venc_->time_base);
		f->pict_type = AV_PICTURE_TYPE_NONE;
		bool ok = encode(venc_.get(), vst_, f.get());
		av_frame_unref(f.get());
		if (!ok)
			return false;
	}
}

bool ShareJob::handle_video_frame(AVFrame *f)
{
	AVRational tb = in_->streams[vidx_]->time_base;
	int64_t pts = f->best_effort_timestamp != AV_NOPTS_VALUE ? f->best_effort_timestamp : f->pts;
	if (pts == AV_NOPTS_VALUE)
		return true;
	double t = (double)pts * av_q2d(tb);
	if (t < start_ - 0.001)
		return true;
	if (t > end_) {
		video_eof_ = true;
		return true;
	}
	last_video_t_ = t;

	if (!vgraph_ && !init_video_graph(f))
		return false;

	f->pts = pts - (int64_t)std::llround(start_ / av_q2d(tb));
	int ret = av_buffersrc_add_frame_flags(vsrc_, f, AV_BUFFERSRC_FLAG_KEEP_REF);
	if (ret < 0) {
		err_ = "Video filter failed: " + av_error(ret);
		return false;
	}
	return pull_video(false);
}

bool ShareJob::decode_video(AVPacket *pkt)
{
	int ret = avcodec_send_packet(vdec_.get(), pkt);
	if (ret < 0 && ret != AVERROR(EAGAIN) && ret != AVERROR_EOF)
		return true; /* skip a broken packet */
	FramePtr f = make_frame();
	for (;;) {
		ret = avcodec_receive_frame(vdec_.get(), f.get());
		if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF)
			return true;
		if (ret < 0)
			return true;
		bool ok = handle_video_frame(f.get());
		av_frame_unref(f.get());
		if (!ok)
			return false;
	}
}

bool ShareJob::pull_audio()
{
	if (!asink_)
		return true;
	FramePtr f = make_frame();
	AVRational sink_tb = av_buffersink_get_time_base(asink_);
	for (;;) {
		int ret = av_buffersink_get_frame(asink_, f.get());
		if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF)
			return true;
		if (ret < 0) {
			err_ = "Audio filter failed";
			return false;
		}
		if (f->pts != AV_NOPTS_VALUE)
			f->pts = av_rescale_q(f->pts, sink_tb, aenc_->time_base);
		bool ok = encode(aenc_.get(), ast_, f.get());
		av_frame_unref(f.get());
		if (!ok)
			return false;
	}
}

bool ShareJob::handle_audio_frame(size_t idx, AVFrame *f)
{
	AVRational tb = in_->streams[aidx_[idx]]->time_base;
	int64_t pts = f->best_effort_timestamp != AV_NOPTS_VALUE ? f->best_effort_timestamp : f->pts;
	if (pts == AV_NOPTS_VALUE)
		return true;
	double t = (double)pts * av_q2d(tb);
	if (t < start_ - 0.001)
		return true;
	if (t > end_) {
		if (!audio_eof_[idx]) {
			audio_eof_[idx] = true;
			(void)av_buffersrc_add_frame_flags(asrcs_[idx], nullptr, 0);
		}
		return pull_audio();
	}
	f->pts = pts - (int64_t)std::llround(start_ / av_q2d(tb));
	int ret = av_buffersrc_add_frame_flags(asrcs_[idx], f, AV_BUFFERSRC_FLAG_KEEP_REF);
	if (ret < 0) {
		err_ = "Audio filter failed: " + av_error(ret);
		return false;
	}
	return pull_audio();
}

bool ShareJob::decode_audio(size_t idx, AVPacket *pkt)
{
	int ret = avcodec_send_packet(adecs_[idx].get(), pkt);
	if (ret < 0 && ret != AVERROR(EAGAIN) && ret != AVERROR_EOF)
		return true;
	FramePtr f = make_frame();
	for (;;) {
		ret = avcodec_receive_frame(adecs_[idx].get(), f.get());
		if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF || ret < 0)
			return true;
		bool ok = audio_eof_[idx] || handle_audio_frame(idx, f.get());
		av_frame_unref(f.get());
		if (!ok)
			return false;
	}
}

bool ShareJob::run(const std::function<bool(float)> &progress, std::string *err, int64_t *bytes)
{
	auto fail = [&]() {
		if (err)
			*err = err_.empty() ? "Export failed" : err_;
		close_output(true);
		return false;
	};

	start_ = std::max(0.0, req_.start);
	end_ = req_.end > 0 ? req_.end : 1e18;

	if (!open_decoders() || !open_encoders() || !open_output() || !init_audio_graph())
		return fail();

	if (start_ > 0.25)
		av_seek_frame(in_.get(), -1, (int64_t)(start_ * AV_TIME_BASE), AVSEEK_FLAG_BACKWARD);

	PacketPtr pkt = make_packet();
	const double span = std::max(0.1, plan_.duration);
	int counter = 0;

	while (av_read_frame(in_.get(), pkt.get()) >= 0) {
		bool ok = true;
		int si = pkt->stream_index;
		if (si == vidx_) {
			if (!video_eof_)
				ok = decode_video(pkt.get());
		} else {
			for (size_t i = 0; i < aidx_.size(); i++)
				if (aidx_[i] == si && !audio_eof_[i])
					ok = decode_audio(i, pkt.get());
		}
		av_packet_unref(pkt.get());
		if (!ok)
			return fail();

		bool audio_done = std::all_of(audio_eof_.begin(), audio_eof_.end(), [](bool b) { return b; });
		if (video_eof_ && audio_done)
			break;

		if (progress && (++counter % 16) == 0) {
			float p = (float)std::clamp((last_video_t_ - start_) / span, 0.0, 1.0);
			if (!progress(p * 0.98f)) {
				err_ = "Cancelled";
				return fail();
			}
		}
	}

	/* drain decoders */
	if (!video_eof_ && !decode_video(nullptr))
		return fail();
	for (size_t i = 0; i < adecs_.size(); i++)
		if (!audio_eof_[i] && !decode_audio(i, nullptr))
			return fail();

	/* drain filters */
	if (vsrc_) {
		(void)av_buffersrc_add_frame_flags(vsrc_, nullptr, 0);
		if (!pull_video(true))
			return fail();
	}
	for (size_t i = 0; i < asrcs_.size(); i++)
		if (!audio_eof_[i])
			(void)av_buffersrc_add_frame_flags(asrcs_[i], nullptr, 0);
	if (!pull_audio())
		return fail();

	/* drain encoders */
	if (!encode(venc_.get(), vst_, nullptr))
		return fail();
	if (aenc_ && !encode(aenc_.get(), ast_, nullptr))
		return fail();

	int ret = av_write_trailer(oc_);
	close_output(false);
	if (ret < 0) {
		err_ = "Could not finish the MP4: " + av_error(ret);
		DeleteFileW(widen(tmp_path_).c_str());
		return fail();
	}
	if (!MoveFileExW(widen(tmp_path_).c_str(), widen(req_.output).c_str(), MOVEFILE_REPLACE_EXISTING)) {
		err_ = "Could not rename the share copy";
		DeleteFileW(widen(tmp_path_).c_str());
		return fail();
	}

	std::error_code ec;
	int64_t size = (int64_t)std::filesystem::file_size(widen(req_.output), ec);
	if (bytes)
		*bytes = size;
	if (progress)
		progress(1.0f);
	return true;
}

} // namespace

bool run_share(const ShareRequest &req, SharePlan plan, const std::function<bool(float)> &progress, std::string *err,
	       int64_t *out_bytes)
{
	/* How big the selected part of the original is, and whether it is already in the asked-for codec. */
	MediaInfo src = probe(req.input);
	double sel_end = req.end > 0 ? std::min(req.end, src.duration) : src.duration;
	double sel_len = std::max(0.1, sel_end - std::max(0.0, req.start));
	int64_t src_part = src.duration > 0 ? (int64_t)((double)src.file_size * sel_len / src.duration) : 0;
	std::string src_codec = to_lower(src.video_codec);
	const bool same_codec = src_codec == plan.codec;

	for (int attempt = 0; attempt < 2; attempt++) {
		int64_t bytes = 0;
		ShareJob job(req, plan);
		if (!job.run(progress, err, &bytes))
			return false;
		/* Re-encoding an already small clip can make it bigger. If the original part
		 * fits the limit and is in the same format, a lossless copy is the better result. */
		if (same_codec && src_part > 0 && bytes > src_part &&
		    (plan.target_bytes <= 0 || src_part <= plan.target_bytes)) {
			TrimOptions opt;
			opt.start = req.start;
			opt.end = req.end;
			opt.audio_streams = req.audio_streams;
			opt.format = "mp4";
			std::string e2;
			if (trim_lossless(req.input, req.output, opt, &e2)) {
				std::error_code ec;
				bytes = (int64_t)std::filesystem::file_size(widen(req.output), ec);
				HL_INFO("Share copy: the original was already smaller, used a lossless copy");
			}
		}
		if (plan.target_bytes > 0 && bytes > plan.target_bytes && plan.video_bitrate > 0 && attempt == 0) {
			double scale = (double)plan.target_bytes / (double)bytes * 0.93;
			plan.video_bitrate = std::max<int64_t>(100000, (int64_t)(plan.video_bitrate * scale));
			HL_INFO("Share copy was %lld bytes (limit %lld), retrying at %lld bps", (long long)bytes,
				(long long)plan.target_bytes, (long long)plan.video_bitrate);
			continue;
		}
		if (out_bytes)
			*out_bytes = bytes;
		return true;
	}
	return false;
}

bool copy_file_to_clipboard(const std::string &path)
{
	std::wstring w = widen(path);
	const size_t bytes = sizeof(DROPFILES) + (w.size() + 2) * sizeof(wchar_t);
	HGLOBAL drop = GlobalAlloc(GHND, bytes);
	if (!drop)
		return false;
	auto *df = (DROPFILES *)GlobalLock(drop);
	df->pFiles = sizeof(DROPFILES);
	df->fWide = TRUE;
	memcpy((uint8_t *)df + sizeof(DROPFILES), w.c_str(), w.size() * sizeof(wchar_t));
	GlobalUnlock(drop);

	HGLOBAL effect = GlobalAlloc(GHND, sizeof(DWORD));
	if (effect) {
		*(DWORD *)GlobalLock(effect) = DROPEFFECT_COPY;
		GlobalUnlock(effect);
	}

	if (!OpenClipboard(nullptr)) {
		GlobalFree(drop);
		if (effect)
			GlobalFree(effect);
		return false;
	}
	EmptyClipboard();
	bool ok = SetClipboardData(CF_HDROP, drop) != nullptr;
	if (effect) {
		UINT fmt = RegisterClipboardFormatW(CFSTR_PREFERREDDROPEFFECT);
		if (!SetClipboardData(fmt, effect))
			GlobalFree(effect);
	}
	CloseClipboard();
	if (!ok)
		GlobalFree(drop);
	return ok;
}

} // namespace hl::media

#include "audio_mix.h"
#include "av_util.h"

#include "../common/log.h"
#include "../common/strings.h"

#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace hl::media {

namespace {

struct Track {
	int in_index = -1;
	CodecPtr dec;
	AVFilterContext *src = nullptr;
	float gain = 1.0f;
	std::string title;
};

struct Out {
	AVFilterContext *sink = nullptr;
	CodecPtr enc;
	AVStream *st = nullptr;
	int64_t next_pts = 0;
};

} // namespace

bool remix_audio(const std::string &in_path, const std::string &out_path, const AudioMixOptions &opt, std::string *err)
{
	auto fail = [&](const std::string &msg) {
		if (err)
			*err = msg;
		return false;
	};

	InputPtr in = open_input(in_path, err);
	if (!in)
		return false;
	AVFormatContext *ic = in.get();

	int video_in = -1;
	std::vector<Track> tracks;
	for (unsigned i = 0; i < ic->nb_streams; i++) {
		AVStream *st = ic->streams[i];
		if (st->codecpar->codec_type == AVMEDIA_TYPE_VIDEO && video_in < 0 &&
		    !(st->disposition & AV_DISPOSITION_ATTACHED_PIC)) {
			video_in = (int)i;
		} else if (st->codecpar->codec_type == AVMEDIA_TYPE_AUDIO) {
			Track t;
			t.in_index = (int)i;
			size_t n = tracks.size();
			t.gain = n < opt.gains.size() ? std::clamp(opt.gains[n], 0.0f, 4.0f) : 1.0f;
			AVDictionaryEntry *title = av_dict_get(st->metadata, "title", nullptr, 0);
			if (!title)
				title = av_dict_get(st->metadata, "handler_name", nullptr, 0);
			t.title = title ? title->value : "Track " + std::to_string(n + 1);
			const AVCodec *dec = avcodec_find_decoder(st->codecpar->codec_id);
			if (!dec)
				return fail("No decoder for audio track " + std::to_string(n + 1));
			t.dec.reset(avcodec_alloc_context3(dec));
			avcodec_parameters_to_context(t.dec.get(), st->codecpar);
			t.dec->pkt_timebase = st->time_base;
			if (avcodec_open2(t.dec.get(), dec, nullptr) < 0)
				return fail("Could not open the audio decoder");
			if (t.dec->ch_layout.order == AV_CHANNEL_ORDER_UNSPEC) {
				int ch = t.dec->ch_layout.nb_channels > 0 ? t.dec->ch_layout.nb_channels : 2;
				av_channel_layout_uninit(&t.dec->ch_layout);
				av_channel_layout_default(&t.dec->ch_layout, ch);
			}
			tracks.push_back(std::move(t));
		}
	}
	if (video_in < 0)
		return fail("The file has no video");
	if (tracks.empty())
		return fail("The file has no audio to adjust");

	const bool with_mix = opt.add_mix && tracks.size() > 1;

	/* filter graph: [in_i] volume -> 48 kHz stereo -> split into [s_i] and [m_i]; [m_*] amix -> [mix] */
	GraphPtr graph(avfilter_graph_alloc());
	std::string desc;
	AVFilterInOut *outputs = nullptr; /* graph inputs (our buffer sources) */
	for (size_t i = 0; i < tracks.size(); i++) {
		Track &t = tracks[i];
		char layout[128] = {};
		av_channel_layout_describe(&t.dec->ch_layout, layout, sizeof(layout));
		AVRational tb = ic->streams[t.in_index]->time_base;
		char args[512];
		snprintf(args, sizeof(args), "time_base=%d/%d:sample_rate=%d:sample_fmt=%s:channel_layout=%s", tb.num, tb.den,
			 t.dec->sample_rate, av_get_sample_fmt_name(t.dec->sample_fmt), layout);
		std::string name = "in" + std::to_string(i);
		if (avfilter_graph_create_filter(&t.src, avfilter_get_by_name("abuffer"), name.c_str(), args, nullptr,
						 graph.get()) < 0) {
			avfilter_inout_free(&outputs);
			return fail("Audio filter setup failed");
		}
		AVFilterInOut *io = avfilter_inout_alloc();
		io->name = av_strdup(name.c_str());
		io->filter_ctx = t.src;
		io->pad_idx = 0;
		io->next = outputs;
		outputs = io;

		char vol[32];
		snprintf(vol, sizeof(vol), "%.4f", t.gain);
		desc += "[" + name + "]volume=" + std::string(vol) +
			":precision=float,aformat=sample_fmts=fltp:sample_rates=48000:channel_layouts=stereo";
		if (with_mix)
			desc += ",asplit=2[s" + std::to_string(i) + "][m" + std::to_string(i) + "];";
		else
			desc += "[s" + std::to_string(i) + "];";
	}
	if (with_mix) {
		for (size_t i = 0; i < tracks.size(); i++)
			desc += "[m" + std::to_string(i) + "]";
		desc += "amix=inputs=" + std::to_string(tracks.size()) +
			":normalize=0:duration=longest,aformat=sample_fmts=fltp:sample_rates=48000:channel_layouts=stereo[mix];";
	}
	if (!desc.empty() && desc.back() == ';')
		desc.pop_back();

	/* sinks: mix first, then one per track */
	std::vector<Out> outs(tracks.size() + (with_mix ? 1 : 0));
	AVFilterInOut *inputs = nullptr; /* graph outputs (our buffer sinks) */
	for (size_t k = 0; k < outs.size(); k++) {
		std::string label = with_mix ? (k == 0 ? "mix" : "s" + std::to_string(k - 1)) : "s" + std::to_string(k);
		std::string name = "out_" + label;
		if (avfilter_graph_create_filter(&outs[k].sink, avfilter_get_by_name("abuffersink"), name.c_str(), nullptr,
						 nullptr, graph.get()) < 0) {
			avfilter_inout_free(&outputs);
			avfilter_inout_free(&inputs);
			return fail("Audio sink setup failed");
		}
		AVFilterInOut *io = avfilter_inout_alloc();
		io->name = av_strdup(label.c_str());
		io->filter_ctx = outs[k].sink;
		io->pad_idx = 0;
		io->next = inputs;
		inputs = io;
	}
	int ret = avfilter_graph_parse_ptr(graph.get(), desc.c_str(), &inputs, &outputs, nullptr);
	avfilter_inout_free(&inputs);
	avfilter_inout_free(&outputs);
	if (ret < 0 || (ret = avfilter_graph_config(graph.get(), nullptr)) < 0)
		return fail("Audio filter graph failed: " + av_error(ret));

	/* output file */
	const std::string tmp = out_path + ".partial";
	AVFormatContext *oc = nullptr;
	ret = avformat_alloc_output_context2(&oc, nullptr, opt.format.c_str(), tmp.c_str());
	if (ret < 0 || !oc)
		return fail("Could not create the output: " + av_error(ret));
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

	AVStream *vin = ic->streams[video_in];
	AVStream *vout = avformat_new_stream(oc, nullptr);
	avcodec_parameters_copy(vout->codecpar, vin->codecpar);
	vout->codecpar->codec_tag = 0;
	if (is_mov && vout->codecpar->codec_id == AV_CODEC_ID_HEVC)
		vout->codecpar->codec_tag = MKTAG('h', 'v', 'c', '1');
	vout->time_base = vin->time_base;
	vout->avg_frame_rate = vin->avg_frame_rate;
	av_dict_copy(&vout->metadata, vin->metadata, 0);

	const AVCodec *aac = avcodec_find_encoder(AV_CODEC_ID_AAC);
	if (!aac) {
		cleanup(true);
		return fail("No AAC encoder");
	}
	for (size_t k = 0; k < outs.size(); k++) {
		Out &o = outs[k];
		o.enc.reset(avcodec_alloc_context3(aac));
		o.enc->sample_rate = 48000;
		o.enc->sample_fmt = AV_SAMPLE_FMT_FLTP;
		av_channel_layout_default(&o.enc->ch_layout, 2);
		o.enc->bit_rate = opt.bitrate;
		o.enc->time_base = AVRational{1, 48000};
		if (oc->oformat->flags & AVFMT_GLOBALHEADER)
			o.enc->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
		if ((ret = avcodec_open2(o.enc.get(), aac, nullptr)) < 0) {
			cleanup(true);
			return fail("Could not open the AAC encoder: " + av_error(ret));
		}
		av_buffersink_set_frame_size(o.sink, (unsigned)o.enc->frame_size);
		o.st = avformat_new_stream(oc, nullptr);
		avcodec_parameters_from_context(o.st->codecpar, o.enc.get());
		o.st->time_base = o.enc->time_base;
		std::string title = with_mix ? (k == 0 ? "Mix" : tracks[k - 1].title) : tracks[k].title;
		av_dict_set(&o.st->metadata, "title", title.c_str(), 0);
		av_dict_set(&o.st->metadata, "handler_name", title.c_str(), 0);
		if (k == 0)
			o.st->disposition |= AV_DISPOSITION_DEFAULT;
	}
	for (unsigned i = 0; i < ic->nb_chapters; i++) {
		/* keep chapters (bookmarks) */
		AVChapter *c = ic->chapters[i];
		AVChapter **chs = (AVChapter **)av_realloc_array(oc->chapters, oc->nb_chapters + 1, sizeof(AVChapter *));
		if (!chs)
			break;
		oc->chapters = chs;
		AVChapter *n = (AVChapter *)av_mallocz(sizeof(AVChapter));
		n->id = c->id;
		n->time_base = c->time_base;
		n->start = c->start;
		n->end = c->end;
		av_dict_copy(&n->metadata, c->metadata, 0);
		oc->chapters[oc->nb_chapters++] = n;
	}
	av_dict_copy(&oc->metadata, ic->metadata, 0);

	if (!(oc->oformat->flags & AVFMT_NOFILE) && (ret = avio_open(&oc->pb, tmp.c_str(), AVIO_FLAG_WRITE)) < 0) {
		cleanup(true);
		return fail("Could not create " + tmp + ": " + av_error(ret));
	}
	AVDictionary *mopts = nullptr;
	if (is_mov)
		av_dict_set(&mopts, "movflags", "+faststart", 0);
	ret = avformat_write_header(oc, &mopts);
	av_dict_free(&mopts);
	if (ret < 0) {
		cleanup(true);
		return fail("Could not write the header: " + av_error(ret));
	}

	bool ok = true;
	PacketPtr pkt = make_packet();
	PacketPtr opkt = make_packet();
	FramePtr frame = make_frame();
	FramePtr filtered = make_frame();
	int64_t last_vdts = INT64_MIN;

	auto encode = [&](Out &o, AVFrame *f) -> bool {
		if (f) {
			/* keep each track where it was in time, but never step backwards */
			int64_t pts = o.next_pts;
			if (f->pts != AV_NOPTS_VALUE)
				pts = std::max(pts, av_rescale_q(f->pts, av_buffersink_get_time_base(o.sink), AVRational{1, 48000}));
			f->pts = pts;
			o.next_pts = pts + f->nb_samples;
		}
		if (avcodec_send_frame(o.enc.get(), f) < 0)
			return f == nullptr;
		while (avcodec_receive_packet(o.enc.get(), opkt.get()) >= 0) {
			av_packet_rescale_ts(opkt.get(), o.enc->time_base, o.st->time_base);
			opkt->stream_index = o.st->index;
			if (av_interleaved_write_frame(oc, opkt.get()) < 0)
				return false;
		}
		return true;
	};
	auto drain = [&](bool flush) -> bool {
		for (auto &o : outs) {
			for (;;) {
				int r = av_buffersink_get_frame(o.sink, filtered.get());
				if (r < 0)
					break;
				bool good = encode(o, filtered.get());
				av_frame_unref(filtered.get());
				if (!good)
					return false;
			}
			if (flush && !encode(o, nullptr))
				return false;
		}
		return true;
	};
	auto decode = [&](Track &t, AVPacket *p) -> bool {
		if (avcodec_send_packet(t.dec.get(), p) < 0 && p)
			return true; /* skip a damaged packet */
		while (avcodec_receive_frame(t.dec.get(), frame.get()) >= 0) {
			int r = av_buffersrc_add_frame_flags(t.src, frame.get(), AV_BUFFERSRC_FLAG_KEEP_REF);
			av_frame_unref(frame.get());
			if (r < 0)
				return false;
		}
		return true;
	};

	while (ok && av_read_frame(ic, pkt.get()) >= 0) {
		const int si = pkt->stream_index;
		if (si == video_in) {
			av_packet_rescale_ts(pkt.get(), vin->time_base, vout->time_base);
			pkt->stream_index = vout->index;
			pkt->pos = -1;
			if (pkt->dts != AV_NOPTS_VALUE) {
				if (last_vdts != INT64_MIN && pkt->dts <= last_vdts)
					pkt->dts = last_vdts + 1;
				if (pkt->pts != AV_NOPTS_VALUE && pkt->pts < pkt->dts)
					pkt->pts = pkt->dts;
				last_vdts = pkt->dts;
			}
			ok = av_interleaved_write_frame(oc, pkt.get()) >= 0;
		} else {
			for (auto &t : tracks)
				if (t.in_index == si) {
					ok = decode(t, pkt.get()) && drain(false);
					break;
				}
		}
		av_packet_unref(pkt.get());
	}
	if (ok) {
		for (auto &t : tracks) {
			decode(t, nullptr);
			(void)av_buffersrc_add_frame_flags(t.src, nullptr, 0);
		}
		ok = drain(true);
	}
	if (!ok) {
		cleanup(true);
		return fail("Could not write the new audio");
	}
	av_write_trailer(oc);
	cleanup(false);
	in.reset();

	if (!MoveFileExW(widen(tmp).c_str(), widen(out_path).c_str(), MOVEFILE_REPLACE_EXISTING)) {
		DeleteFileW(widen(tmp).c_str());
		return fail("Could not rename the output file");
	}
	HL_INFO("Audio re-mixed: %zu track(s)%s", tracks.size(), with_mix ? " + mix" : "");
	return true;
}

} // namespace hl::media

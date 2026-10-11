#include "thumbnail.h"
#include "av_util.h"

#include <algorithm>

namespace hl::media {

bool decode_thumbnail(const std::string &path, double at, int max_w, int max_h, std::vector<uint8_t> &bgra, int &w,
		      int &h, std::string *err)
{
	InputPtr in = open_input(path, err);
	if (!in)
		return false;
	AVFormatContext *ic = in.get();
	int vidx = av_find_best_stream(ic, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
	if (vidx < 0) {
		if (err)
			*err = "No video stream";
		return false;
	}
	AVStream *st = ic->streams[vidx];
	const AVCodec *dec = avcodec_find_decoder(st->codecpar->codec_id);
	if (!dec)
		return false;
	CodecPtr ctx(avcodec_alloc_context3(dec));
	avcodec_parameters_to_context(ctx.get(), st->codecpar);
	ctx->pkt_timebase = st->time_base;
	ctx->thread_count = 2;
	if (avcodec_open2(ctx.get(), dec, nullptr) < 0)
		return false;

	if (at > 0.05)
		av_seek_frame(ic, -1, (int64_t)(at * AV_TIME_BASE), AVSEEK_FLAG_BACKWARD);

	PacketPtr pkt = make_packet();
	FramePtr frame = make_frame();
	bool got = false;
	int packets = 0;
	while (!got && av_read_frame(ic, pkt.get()) >= 0 && packets < 2000) {
		if (pkt->stream_index == vidx) {
			packets++;
			if (avcodec_send_packet(ctx.get(), pkt.get()) >= 0) {
				while (avcodec_receive_frame(ctx.get(), frame.get()) >= 0) {
					int64_t pts = frame->best_effort_timestamp;
					double t = pts == AV_NOPTS_VALUE ? at : pts * av_q2d(st->time_base);
					if (t + 0.02 >= at) {
						got = true;
						break;
					}
					av_frame_unref(frame.get());
				}
			}
		}
		av_packet_unref(pkt.get());
	}
	if (!got) {
		avcodec_send_packet(ctx.get(), nullptr);
		got = avcodec_receive_frame(ctx.get(), frame.get()) >= 0;
	}
	if (!got) {
		if (err)
			*err = "Could not decode a frame";
		return false;
	}

	double scale = std::min((double)max_w / frame->width, (double)max_h / frame->height);
	scale = std::min(scale, 1.0);
	w = std::max(2, (int)(frame->width * scale)) & ~1;
	h = std::max(2, (int)(frame->height * scale)) & ~1;

	SwsContext *sws = sws_getContext(frame->width, frame->height, (AVPixelFormat)frame->format, w, h, AV_PIX_FMT_BGRA,
					 SWS_BICUBIC, nullptr, nullptr, nullptr);
	if (!sws)
		return false;
	bgra.resize((size_t)w * h * 4);
	uint8_t *dst[4] = {bgra.data(), nullptr, nullptr, nullptr};
	int dst_stride[4] = {w * 4, 0, 0, 0};
	sws_scale(sws, frame->data, frame->linesize, 0, frame->height, dst, dst_stride);
	sws_freeContext(sws);
	return true;
}

} // namespace hl::media

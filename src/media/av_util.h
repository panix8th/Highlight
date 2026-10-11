#pragma once

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavfilter/avfilter.h>
#include <libavfilter/buffersink.h>
#include <libavfilter/buffersrc.h>
#include <libavformat/avformat.h>
#include <libavutil/avutil.h>
#include <libavutil/channel_layout.h>
#include <libavutil/imgutils.h>
#include <libavutil/opt.h>
#include <libswscale/swscale.h>
}

#include <memory>
#include <string>

namespace hl::media {

std::string av_error(int code);

/* FFmpeg wants UTF-8 paths on Windows; our paths are UTF-8 already. */

struct FormatInputDeleter {
	void operator()(AVFormatContext *ctx) const
	{
		if (ctx)
			avformat_close_input(&ctx);
	}
};
using InputPtr = std::unique_ptr<AVFormatContext, FormatInputDeleter>;

struct CodecDeleter {
	void operator()(AVCodecContext *ctx) const { avcodec_free_context(&ctx); }
};
using CodecPtr = std::unique_ptr<AVCodecContext, CodecDeleter>;

struct FrameDeleter {
	void operator()(AVFrame *f) const { av_frame_free(&f); }
};
using FramePtr = std::unique_ptr<AVFrame, FrameDeleter>;

struct PacketDeleter {
	void operator()(AVPacket *p) const { av_packet_free(&p); }
};
using PacketPtr = std::unique_ptr<AVPacket, PacketDeleter>;

struct GraphDeleter {
	void operator()(AVFilterGraph *g) const { avfilter_graph_free(&g); }
};
using GraphPtr = std::unique_ptr<AVFilterGraph, GraphDeleter>;

inline FramePtr make_frame()
{
	return FramePtr(av_frame_alloc());
}
inline PacketPtr make_packet()
{
	return PacketPtr(av_packet_alloc());
}

InputPtr open_input(const std::string &path, std::string *err = nullptr);

} // namespace hl::media

#include "waveform.h"
#include "av_util.h"

#include <algorithm>
#include <cmath>

namespace hl::media {

static float sample_abs(const AVFrame *f, int ch, int i)
{
	const AVSampleFormat fmt = (AVSampleFormat)f->format;
	const int channels = f->ch_layout.nb_channels;
	switch (fmt) {
	case AV_SAMPLE_FMT_FLTP:
		return std::fabs(((const float *)f->extended_data[ch])[i]);
	case AV_SAMPLE_FMT_FLT:
		return std::fabs(((const float *)f->extended_data[0])[i * channels + ch]);
	case AV_SAMPLE_FMT_S16P:
		return std::fabs(((const int16_t *)f->extended_data[ch])[i] / 32768.0f);
	case AV_SAMPLE_FMT_S16:
		return std::fabs(((const int16_t *)f->extended_data[0])[i * channels + ch] / 32768.0f);
	case AV_SAMPLE_FMT_S32P:
		return std::fabs(((const int32_t *)f->extended_data[ch])[i] / 2147483648.0f);
	case AV_SAMPLE_FMT_S32:
		return std::fabs(((const int32_t *)f->extended_data[0])[i * channels + ch] / 2147483648.0f);
	default:
		return 0.0f;
	}
}

bool audio_peaks(const std::string &path, int stream_index, int buckets, std::vector<float> &out)
{
	out.assign(std::max(1, buckets), 0.0f);
	InputPtr in = open_input(path);
	if (!in)
		return false;
	AVFormatContext *ic = in.get();
	if (stream_index < 0 || stream_index >= (int)ic->nb_streams)
		return false;
	AVStream *st = ic->streams[stream_index];
	if (st->codecpar->codec_type != AVMEDIA_TYPE_AUDIO)
		return false;

	const double duration = ic->duration > 0 ? (double)ic->duration / AV_TIME_BASE : 0;
	if (duration <= 0)
		return false;

	const AVCodec *dec = avcodec_find_decoder(st->codecpar->codec_id);
	if (!dec)
		return false;
	CodecPtr ctx(avcodec_alloc_context3(dec));
	avcodec_parameters_to_context(ctx.get(), st->codecpar);
	ctx->pkt_timebase = st->time_base;
	if (avcodec_open2(ctx.get(), dec, nullptr) < 0)
		return false;

	for (unsigned i = 0; i < ic->nb_streams; i++)
		if ((int)i != stream_index)
			ic->streams[i]->discard = AVDISCARD_ALL;

	PacketPtr pkt = make_packet();
	FramePtr f = make_frame();
	auto consume = [&]() {
		while (avcodec_receive_frame(ctx.get(), f.get()) >= 0) {
			int64_t pts = f->best_effort_timestamp;
			if (pts != AV_NOPTS_VALUE && f->sample_rate > 0) {
				double t0 = pts * av_q2d(st->time_base);
				int channels = f->ch_layout.nb_channels;
				for (int i = 0; i < f->nb_samples; i++) {
					double t = t0 + (double)i / f->sample_rate;
					int b = (int)(t / duration * out.size());
					if (b < 0 || b >= (int)out.size())
						continue;
					float peak = 0;
					for (int ch = 0; ch < channels; ch++)
						peak = std::max(peak, sample_abs(f.get(), ch, i));
					out[b] = std::max(out[b], peak);
				}
			}
			av_frame_unref(f.get());
		}
	};
	while (av_read_frame(ic, pkt.get()) >= 0) {
		if (pkt->stream_index == stream_index && avcodec_send_packet(ctx.get(), pkt.get()) >= 0)
			consume();
		av_packet_unref(pkt.get());
	}
	avcodec_send_packet(ctx.get(), nullptr);
	consume();

	/* soft-knee so quiet tracks are still visible */
	for (float &v : out)
		v = std::min(1.0f, std::sqrt(v));
	return true;
}

} // namespace hl::media

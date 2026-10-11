#include "player.h"
#include "av_util.h"

extern "C" {
#include <libavutil/audio_fifo.h>
#include <libavutil/time.h>
#include <libswresample/swresample.h>
}

#include <algorithm>
#include <chrono>

namespace hl::media {

namespace {

struct AudioIn {
	int index = -1;
	CodecPtr dec;
	SwrContext *swr = nullptr;
	AVAudioFifo *fifo = nullptr;
	bool active = false;
	~AudioIn()
	{
		swr_free(&swr);
		if (fifo)
			av_audio_fifo_free(fifo);
	}
	void reset()
	{
		if (dec)
			avcodec_flush_buffers(dec.get());
		if (fifo)
			av_audio_fifo_reset(fifo);
		if (swr) {
			swr_close(swr);
			swr_init(swr);
		}
	}
};

int64_t now_us()
{
	return av_gettime_relative();
}

constexpr size_t kMaxFrames = 5;
constexpr size_t kAudioTarget = 48000 * 2; /* one second of stereo samples */

} // namespace

struct Player::Impl {
	InputPtr in;
	int vidx = -1;
	CodecPtr vdec;
	AVRational vtb{1, 1000};
	std::vector<std::unique_ptr<AudioIn>> audio;
	SwsContext *sws = nullptr;
	int max_w = 1280, max_h = 720;
	double skip_until = 0;
	~Impl() { sws_freeContext(sws); }
};

Player::Player() = default;

Player::~Player()
{
	close();
}

bool Player::open(const std::string &path, int max_w, int max_h, std::string *err)
{
	close();
	auto im = std::make_unique<Impl>();
	im->max_w = max_w;
	im->max_h = max_h;
	im->in = open_input(path, err);
	if (!im->in)
		return false;
	AVFormatContext *ic = im->in.get();

	im->vidx = av_find_best_stream(ic, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
	if (im->vidx < 0) {
		if (err)
			*err = "No video stream";
		return false;
	}
	AVStream *vst = ic->streams[im->vidx];
	im->vtb = vst->time_base;

	const char *hw = vst->codecpar->codec_id == AV_CODEC_ID_HEVC   ? "hevc_cuvid"
			 : vst->codecpar->codec_id == AV_CODEC_ID_H264 ? "h264_cuvid"
			 : vst->codecpar->codec_id == AV_CODEC_ID_AV1  ? "av1_cuvid"
								       : nullptr;
	if (hw) {
		if (const AVCodec *dec = avcodec_find_decoder_by_name(hw)) {
			CodecPtr ctx(avcodec_alloc_context3(dec));
			avcodec_parameters_to_context(ctx.get(), vst->codecpar);
			ctx->pkt_timebase = vst->time_base;
			if (avcodec_open2(ctx.get(), dec, nullptr) >= 0)
				im->vdec = std::move(ctx);
		}
	}
	if (!im->vdec) {
		const AVCodec *dec = avcodec_find_decoder(vst->codecpar->codec_id);
		if (!dec) {
			if (err)
				*err = "No decoder for this video";
			return false;
		}
		CodecPtr ctx(avcodec_alloc_context3(dec));
		avcodec_parameters_to_context(ctx.get(), vst->codecpar);
		ctx->pkt_timebase = vst->time_base;
		ctx->thread_count = 0;
		if (avcodec_open2(ctx.get(), dec, nullptr) < 0) {
			if (err)
				*err = "Could not open the video decoder";
			return false;
		}
		im->vdec = std::move(ctx);
	}

	AVChannelLayout stereo = AV_CHANNEL_LAYOUT_STEREO;
	for (unsigned i = 0; i < ic->nb_streams; i++) {
		AVStream *st = ic->streams[i];
		if (st->codecpar->codec_type != AVMEDIA_TYPE_AUDIO)
			continue;
		const AVCodec *dec = avcodec_find_decoder(st->codecpar->codec_id);
		if (!dec)
			continue;
		auto a = std::make_unique<AudioIn>();
		a->index = (int)i;
		a->dec.reset(avcodec_alloc_context3(dec));
		avcodec_parameters_to_context(a->dec.get(), st->codecpar);
		a->dec->pkt_timebase = st->time_base;
		if (avcodec_open2(a->dec.get(), dec, nullptr) < 0)
			continue;
		if (a->dec->ch_layout.order == AV_CHANNEL_ORDER_UNSPEC) {
			int ch = a->dec->ch_layout.nb_channels > 0 ? a->dec->ch_layout.nb_channels : 2;
			av_channel_layout_uninit(&a->dec->ch_layout);
			av_channel_layout_default(&a->dec->ch_layout, ch);
		}
		if (swr_alloc_set_opts2(&a->swr, &stereo, AV_SAMPLE_FMT_FLT, 48000, &a->dec->ch_layout,
					a->dec->sample_fmt, a->dec->sample_rate, 0, nullptr) < 0 ||
		    swr_init(a->swr) < 0)
			continue;
		a->fifo = av_audio_fifo_alloc(AV_SAMPLE_FMT_FLT, 2, 48000);
		im->audio.push_back(std::move(a));
	}

	duration_ = ic->duration > 0 ? (double)ic->duration / AV_TIME_BASE : 0;

	{
		std::lock_guard lock(mutex_);
		impl_ = std::move(im);
		clock_pos_ = 0;
		clock_start_us_ = now_us();
		playing_ = false;
		seek_pending_ = true;
		seek_target_ = 0;
		audio_streams_.clear();
		if (!impl_->audio.empty())
			audio_streams_.push_back(impl_->audio.front()->index);
		streams_changed_ = true;
		frames_.clear();
		audio_.clear();
		still_ready_ = false;
		eof_ = false;
	}
	running_ = true;
	open_ = true;
	thread_ = std::thread(&Player::worker, this);
	return true;
}

void Player::close()
{
	running_ = false;
	cv_.notify_all();
	if (thread_.joinable())
		thread_.join();
	std::lock_guard lock(mutex_);
	impl_.reset();
	frames_.clear();
	audio_.clear();
	still_ready_ = false;
	playing_ = false;
	open_ = false;
}

double Player::position() const
{
	std::lock_guard lock(mutex_);
	if (playing_)
		return std::min(duration_, clock_pos_ + (double)(now_us() - clock_start_us_) / 1e6);
	return clock_pos_;
}

void Player::play()
{
	std::lock_guard lock(mutex_);
	if (!open_)
		return;
	if (eof_ || clock_pos_ >= duration_ - 0.05) {
		clock_pos_ = 0;
		seek_pending_ = true;
		seek_target_ = 0;
		frames_.clear();
		audio_.clear();
		eof_ = false;
	}
	clock_start_us_ = now_us();
	playing_ = true;
	cv_.notify_all();
}

void Player::pause()
{
	std::lock_guard lock(mutex_);
	if (playing_)
		clock_pos_ = std::min(duration_, clock_pos_ + (double)(now_us() - clock_start_us_) / 1e6);
	playing_ = false;
}

void Player::seek(double t)
{
	std::lock_guard lock(mutex_);
	t = std::clamp(t, 0.0, std::max(0.0, duration_ - 0.01));
	clock_pos_ = t;
	clock_start_us_ = now_us();
	seek_pending_ = true;
	seek_target_ = t;
	frames_.clear();
	audio_.clear();
	eof_ = false;
	cv_.notify_all();
}

void Player::set_audio_streams(const std::vector<int> &streams)
{
	std::lock_guard lock(mutex_);
	audio_streams_ = streams;
	streams_changed_ = true;
	audio_.clear();
	cv_.notify_all();
}

bool Player::take_frame(std::vector<uint8_t> &bgra, int &w, int &h)
{
	std::lock_guard lock(mutex_);
	if (still_ready_) {
		still_ready_ = false;
		bgra = still_.bgra;
		w = still_.w;
		h = still_.h;
		return true;
	}
	if (!playing_)
		return false;

	double pos = std::min(duration_, clock_pos_ + (double)(now_us() - clock_start_us_) / 1e6);
	bool got = false;
	Frame f;
	while (!frames_.empty() && frames_.front().pts <= pos) {
		f = std::move(frames_.front());
		frames_.pop_front();
		got = true;
	}
	if (got) {
		bgra = std::move(f.bgra);
		w = f.w;
		h = f.h;
	}
	if (eof_ && frames_.empty() && pos >= duration_ - 0.05) {
		clock_pos_ = duration_;
		playing_ = false;
	}
	cv_.notify_all();
	return got;
}

size_t Player::read_audio(float *dst, size_t frames)
{
	std::lock_guard lock(mutex_);
	if (!playing_)
		return 0;
	size_t n = std::min(frames, audio_.size() / 2);
	const float vol = volume_;
	for (size_t i = 0; i < n * 2; i++) {
		dst[i] = audio_.front() * vol;
		audio_.pop_front();
	}
	if (n > 0)
		cv_.notify_all();
	return n;
}

void Player::worker()
{
	Impl *im = impl_.get();
	AVFormatContext *ic = im->in.get();
	PacketPtr pkt = make_packet();
	FramePtr frame = make_frame();
	std::vector<float> mix, tmp;

	auto convert = [&](const AVFrame *src, Frame &out) {
		double scale = std::min({(double)im->max_w / src->width, (double)im->max_h / src->height, 1.0});
		int w = std::max(2, (int)(src->width * scale)) & ~1;
		int h = std::max(2, (int)(src->height * scale)) & ~1;
		im->sws = sws_getCachedContext(im->sws, src->width, src->height, (AVPixelFormat)src->format, w, h,
					       AV_PIX_FMT_BGRA, SWS_BILINEAR, nullptr, nullptr, nullptr);
		if (!im->sws)
			return false;
		out.w = w;
		out.h = h;
		out.bgra.resize((size_t)w * h * 4);
		uint8_t *dst[4] = {out.bgra.data(), nullptr, nullptr, nullptr};
		int stride[4] = {w * 4, 0, 0, 0};
		sws_scale(im->sws, src->data, src->linesize, 0, src->height, dst, stride);
		return true;
	};

	auto frame_time = [&](const AVFrame *f, AVRational tb) {
		int64_t pts = f->best_effort_timestamp != AV_NOPTS_VALUE ? f->best_effort_timestamp : f->pts;
		return pts == AV_NOPTS_VALUE ? -1.0 : pts * av_q2d(tb);
	};

	auto mix_out = [&]() {
		int n = INT32_MAX;
		int active = 0;
		for (auto &a : im->audio)
			if (a->active) {
				n = std::min(n, av_audio_fifo_size(a->fifo));
				active++;
			}
		if (active == 0 || n <= 0 || n == INT32_MAX)
			return;
		mix.assign((size_t)n * 2, 0.0f);
		tmp.resize((size_t)n * 2);
		for (auto &a : im->audio) {
			if (!a->active)
				continue;
			void *ptr = tmp.data();
			av_audio_fifo_read(a->fifo, &ptr, n);
			for (size_t i = 0; i < mix.size(); i++)
				mix[i] += tmp[i];
		}
		std::lock_guard lock(mutex_);
		for (float s : mix)
			audio_.push_back(std::clamp(s, -1.0f, 1.0f));
	};

	/* seeking: decode until the frame at the target, publish it as a still */
	bool seeking = false;
	double target = 0;

	auto handle_video = [&](AVPacket *p) {
		if (avcodec_send_packet(im->vdec.get(), p) < 0 && p)
			return;
		while (avcodec_receive_frame(im->vdec.get(), frame.get()) >= 0) {
			double t = frame_time(frame.get(), im->vtb);
			if (seeking) {
				if (t + 0.008 >= target || t < 0) {
					Frame f;
					f.pts = t;
					if (convert(frame.get(), f)) {
						std::lock_guard lock(mutex_);
						still_ = std::move(f);
						still_ready_ = true;
					}
					seeking = false;
				}
			} else if (t + 0.008 >= im->skip_until) {
				Frame f;
				f.pts = t;
				if (convert(frame.get(), f)) {
					std::lock_guard lock(mutex_);
					frames_.push_back(std::move(f));
				}
			}
			av_frame_unref(frame.get());
		}
	};

	auto handle_audio = [&](AudioIn &a, AVPacket *p) {
		if (avcodec_send_packet(a.dec.get(), p) < 0 && p)
			return;
		AVRational tb = ic->streams[a.index]->time_base;
		while (avcodec_receive_frame(a.dec.get(), frame.get()) >= 0) {
			double t = frame_time(frame.get(), tb);
			if (t >= 0 && t + 0.01 < im->skip_until) {
				av_frame_unref(frame.get());
				continue;
			}
			int out_n = swr_get_out_samples(a.swr, frame->nb_samples);
			if (out_n > 0) {
				tmp.resize((size_t)out_n * 2);
				uint8_t *out = (uint8_t *)tmp.data();
				int got = swr_convert(a.swr, &out, out_n, (const uint8_t **)frame->extended_data,
						      frame->nb_samples);
				if (got > 0) {
					void *ptr = tmp.data();
					av_audio_fifo_write(a.fifo, &ptr, got);
				}
			}
			av_frame_unref(frame.get());
		}
	};

	while (running_) {
		bool do_seek = false, playing = false, streams_changed = false;
		std::vector<int> streams;
		{
			std::unique_lock lock(mutex_);
			auto need_more = [&]() {
				bool any_audio = false;
				for (auto &a : im->audio)
					any_audio |= a->active;
				return !eof_ && frames_.size() < kMaxFrames && (!any_audio || audio_.size() < kAudioTarget * 2);
			};
			if (!seeking)
				cv_.wait_for(lock, std::chrono::milliseconds(15), [&] {
					return !running_ || seek_pending_ || streams_changed_ || (playing_ && need_more());
				});
			if (!running_)
				break;
			do_seek = seek_pending_;
			seek_pending_ = false;
			if (do_seek)
				target = seek_target_;
			playing = playing_ && need_more();
			streams_changed = streams_changed_;
			streams_changed_ = false;
			streams = audio_streams_;
		}

		if (streams_changed) {
			for (auto &a : im->audio) {
				bool want = std::find(streams.begin(), streams.end(), a->index) != streams.end();
				if (want != a->active) {
					a->active = want;
					a->reset();
				}
			}
			for (auto &a : im->audio)
				if (a->active && a->fifo)
					av_audio_fifo_reset(a->fifo);
		}

		if (do_seek) {
			av_seek_frame(ic, -1, (int64_t)(target * AV_TIME_BASE), AVSEEK_FLAG_BACKWARD);
			avcodec_flush_buffers(im->vdec.get());
			for (auto &a : im->audio)
				a->reset();
			im->skip_until = target;
			seeking = true;
			{
				std::lock_guard lock(mutex_);
				eof_ = false;
			}
		}

		if (!seeking && !playing)
			continue;

		/* read and decode a burst of packets */
		for (int burst = 0; burst < 8 && running_; burst++) {
			int r = av_read_frame(ic, pkt.get());
			if (r < 0) {
				handle_video(nullptr);
				for (auto &a : im->audio)
					if (a->active)
						handle_audio(*a, nullptr);
				mix_out();
				seeking = false;
				std::lock_guard lock(mutex_);
				eof_ = true;
				break;
			}
			if (pkt->stream_index == im->vidx) {
				handle_video(pkt.get());
			} else {
				for (auto &a : im->audio)
					if (a->active && a->index == pkt->stream_index)
						handle_audio(*a, pkt.get());
				mix_out();
			}
			av_packet_unref(pkt.get());
			if (!seeking && burst >= 2)
				break;
		}
	}
}

} // namespace hl::media

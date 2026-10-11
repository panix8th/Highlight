/* End-to-end test of the clip path: encoded packets -> DiskRing -> write_clip -> MP4.
 *
 * Replays a real recording (video + audio tracks) into the rolling buffer the way
 * libobs delivers packets (frame/sample timestamps, system-clock sys_dts_usec,
 * separate start times for audio and video), saves clips while packets keep
 * arriving, then opens every clip again and checks it is readable, decodable
 * and as long as requested.
 *
 * usage: clip_pipeline_test <input.mkv> <work dir> [--annexb] [--obs-headers [--no-header-fix]] [--speed N]
 *
 *   --annexb         video packets in Annex B, as hardware encoders deliver them
 *   --obs-headers    like libobs + NVENC: no codec headers up front, parameter sets
 *                    only inside the very first packet (implies --annexb)
 *   --no-header-fix  do not keep the headers from that first packet (old behaviour)
 */

#include "core/clip_writer.h"
#include "core/disk_ring.h"
#include "common/strings.h"
#include "media/av_util.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavcodec/bsf.h>
#include <libavformat/avformat.h>
}

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

using namespace hl;
namespace fs = std::filesystem;

static std::string codec_name(AVCodecID id)
{
	switch (id) {
	case AV_CODEC_ID_H264:
		return "h264";
	case AV_CODEC_ID_HEVC:
		return "hevc";
	case AV_CODEC_ID_AV1:
		return "av1";
	case AV_CODEC_ID_AAC:
		return "aac";
	case AV_CODEC_ID_OPUS:
		return "opus";
	default:
		return "";
	}
}

struct Press {
	double at;    /* media seconds */
	int pre;
	int post;
	bool queued = false;
	bool done = false;
	ClipResult result;
};

/* Opens a finished clip and decodes every video packet. */
static bool verify(const std::string &path, double want_seconds, size_t want_streams, std::string *why)
{
	AVFormatContext *fmt = nullptr;
	int ret = avformat_open_input(&fmt, path.c_str(), nullptr, nullptr);
	if (ret < 0) {
		*why = "open: " + media::av_error(ret);
		return false;
	}
	ret = avformat_find_stream_info(fmt, nullptr);
	if (ret < 0) {
		*why = "stream info: " + media::av_error(ret);
		avformat_close_input(&fmt);
		return false;
	}
	if (fmt->nb_streams != want_streams) {
		*why = "expected " + std::to_string(want_streams) + " streams, got " + std::to_string(fmt->nb_streams);
		avformat_close_input(&fmt);
		return false;
	}
	int vs = av_find_best_stream(fmt, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
	const AVCodec *dec = vs >= 0 ? avcodec_find_decoder(fmt->streams[vs]->codecpar->codec_id) : nullptr;
	AVCodecContext *cc = dec ? avcodec_alloc_context3(dec) : nullptr;
	if (!cc || avcodec_parameters_to_context(cc, fmt->streams[vs]->codecpar) < 0 || avcodec_open2(cc, dec, nullptr) < 0) {
		*why = "no video decoder";
		avformat_close_input(&fmt);
		return false;
	}
	AVPacket *pkt = av_packet_alloc();
	AVFrame *frame = av_frame_alloc();
	int frames = 0, errors = 0;
	while (av_read_frame(fmt, pkt) >= 0) {
		if (pkt->stream_index == vs) {
			if (avcodec_send_packet(cc, pkt) < 0)
				errors++;
			while (avcodec_receive_frame(cc, frame) >= 0)
				frames++;
		}
		av_packet_unref(pkt);
	}
	avcodec_send_packet(cc, nullptr);
	while (avcodec_receive_frame(cc, frame) >= 0)
		frames++;
	double fps = av_q2d(fmt->streams[vs]->avg_frame_rate);
	double duration = fmt->duration > 0 ? fmt->duration / (double)AV_TIME_BASE : 0;
	av_frame_free(&frame);
	av_packet_free(&pkt);
	avcodec_free_context(&cc);
	avformat_close_input(&fmt);

	char buf[256];
	snprintf(buf, sizeof(buf), "%.2f s, %d frames (%.1f fps), %d decode errors", duration, frames, fps, errors);
	*why = buf;
	/* Starts on the keyframe at or before the requested start: up to one GOP longer. */
	if (duration < want_seconds - 0.5 || duration > want_seconds + 2.5)
		return false;
	if (fps > 0 && std::fabs(frames / fps - want_seconds) > 2.5)
		return false;
	return errors == 0;
}

/* Removes parameter-set NAL units from an Annex B packet. */
static void strip_parameter_sets(const std::string &codec, const uint8_t *data, size_t size, std::vector<uint8_t> *out)
{
	const bool hevc = codec == "hevc";
	out->clear();
	size_t i = 0;
	auto start_at = [&](size_t k, size_t *len) {
		if (k + 3 <= size && data[k] == 0 && data[k + 1] == 0 && data[k + 2] == 1) {
			*len = 3;
			return true;
		}
		if (k + 4 <= size && data[k] == 0 && data[k + 1] == 0 && data[k + 2] == 0 && data[k + 3] == 1) {
			*len = 4;
			return true;
		}
		return false;
	};
	while (i < size) {
		size_t sc = 0;
		if (!start_at(i, &sc)) {
			i++;
			continue;
		}
		size_t nal = i + sc, j = nal, sc2 = 0;
		while (j < size && !start_at(j, &sc2))
			j++;
		int type = nal < size ? (hevc ? (data[nal] >> 1) & 0x3f : data[nal] & 0x1f) : -1;
		bool ps = hevc ? (type >= 32 && type <= 34) : (type == 7 || type == 8);
		if (!ps)
			out->insert(out->end(), data + i, data + j);
		i = j;
	}
}

int wmain(int argc, wchar_t **argv)
{
	if (argc < 3) {
		fprintf(stderr, "usage: clip_pipeline_test <input> <work dir> [--annexb] [--speed N]\n");
		return 2;
	}
	const std::string input = narrow(argv[1]);
	const fs::path work = argv[2];
	bool annexb = false, obs_headers = false, header_fix = true;
	double speed = 8.0;
	for (int i = 3; i < argc; i++) {
		std::wstring a = argv[i];
		if (a == L"--annexb")
			annexb = true;
		else if (a == L"--obs-headers")
			annexb = obs_headers = true;
		else if (a == L"--no-header-fix")
			header_fix = false;
		else if (a == L"--speed" && i + 1 < argc)
			speed = _wtof(argv[++i]);
	}
	std::error_code ec;
	fs::remove_all(work, ec);
	fs::create_directories(work / "clips", ec);

	AVFormatContext *in = nullptr;
	if (avformat_open_input(&in, input.c_str(), nullptr, nullptr) < 0 || avformat_find_stream_info(in, nullptr) < 0) {
		fprintf(stderr, "cannot open %s\n", input.c_str());
		return 2;
	}

	/* Streams as libobs would describe them. */
	ClipJob base;
	base.track_to_stream.assign(kMaxClipTracks, -1);
	std::vector<int> track_of(in->nb_streams, -1); /* input stream -> audio track */
	int video_in = -1;
	AVBSFContext *bsf = nullptr;
	int fps = 60;
	for (unsigned i = 0; i < in->nb_streams; i++) {
		AVCodecParameters *par = in->streams[i]->codecpar;
		media::StreamDesc d;
		d.codec = codec_name(par->codec_id);
		if (par->codec_type == AVMEDIA_TYPE_VIDEO && video_in < 0) {
			video_in = (int)i;
			d.video = true;
			d.width = par->width;
			d.height = par->height;
			AVRational fr = in->streams[i]->avg_frame_rate;
			fps = fr.den ? (int)llround(av_q2d(fr)) : 60;
			d.fps_num = fps;
			d.fps_den = 1;
			if (annexb) {
				const AVBitStreamFilter *f =
					av_bsf_get_by_name(par->codec_id == AV_CODEC_ID_HEVC ? "hevc_mp4toannexb" : "h264_mp4toannexb");
				av_bsf_alloc(f, &bsf);
				avcodec_parameters_copy(bsf->par_in, par);
				bsf->time_base_in = in->streams[i]->time_base;
				av_bsf_init(bsf);
				par = bsf->par_out;
			}
			if (!obs_headers)
				d.extradata.assign(par->extradata, par->extradata + par->extradata_size);
			base.streams.insert(base.streams.begin(), d);
		} else if (par->codec_type == AVMEDIA_TYPE_AUDIO) {
			size_t track = 0;
			for (int t : track_of)
				if (t >= 0)
					track++;
			if (track >= kMaxClipTracks)
				continue;
			track_of[i] = (int)track;
			d.sample_rate = par->sample_rate;
			d.channels = par->ch_layout.nb_channels;
			d.frame_size = par->frame_size ? par->frame_size : 1024;
			d.extradata.assign(par->extradata, par->extradata + par->extradata_size);
			d.title = "Track " + std::to_string(track + 1);
			base.streams.push_back(d);
		}
	}
	if (video_in < 0) {
		fprintf(stderr, "no video stream\n");
		return 2;
	}
	for (unsigned i = 0; i < in->nb_streams; i++)
		if (track_of[i] >= 0)
			base.track_to_stream[track_of[i]] = 1 + track_of[i];
	base.req.format = "mp4";

	DiskRing ring;
	std::string err;
	if (!ring.open((work / "ring").wstring(), 90'000'000, &err)) {
		fprintf(stderr, "ring: %s\n", err.c_str());
		return 2;
	}

	/* libobs: sys_dts_usec = encoder start (os clock) + dts in microseconds. */
	const int64_t video_start_sys = 5'000'000'000LL;
	const int64_t audio_start_sys = video_start_sys + 23'000; /* audio starts a little later */

	std::vector<Press> presses = {{20, 30, 5}, {40, 30, 5}, {75, 30, 5}, {76, 30, 5}, {100, 10, 0}, {118, 60, 5}, {141, 30, 5}};
	std::mutex mu;
	std::atomic<int64_t> newest_media_usec{0};
	std::atomic<bool> producing{true};

	std::thread saver([&] {
		for (;;) {
			Press *next = nullptr;
			{
				std::lock_guard lock(mu);
				for (auto &p : presses)
					if (p.queued && !p.done) {
						next = &p;
						break;
					}
			}
			if (!next) {
				if (!producing) {
					std::lock_guard lock(mu);
					bool left = false;
					for (auto &p : presses)
						left |= !p.done;
					if (!left)
						return;
					for (auto &p : presses)
						if (!p.queued)
							p.queued = true; /* stream ended: flush like on_stop */
					continue;
				}
				std::this_thread::sleep_for(std::chrono::milliseconds(20));
				continue;
			}
			ClipJob job;
			{
				std::lock_guard lock(mu);
				job = base;
			}
			job.req.id = (uint64_t)(next - presses.data()) + 1;
			job.req.press_sys_usec = video_start_sys + (int64_t)(next->at * 1e6);
			job.req.pre_seconds = next->pre;
			job.req.post_seconds = next->post;
			char name[64];
			snprintf(name, sizeof(name), "clip_%02d_at_%.0f.mp4", (int)job.req.id, next->at);
			job.req.path = narrow((work / "clips" / name).wstring());
			ClipResult r = write_clip(ring, job);
			std::lock_guard lock(mu);
			next->result = r;
			next->done = true;
		}
	});

	/* Producer: interleaved packets, paced at `speed` x real time. */
	AVPacket *pkt = av_packet_alloc();
	bool first_video_seen = false;
	const auto t0 = std::chrono::steady_clock::now();
	uint64_t pushed = 0;
	while (av_read_frame(in, pkt) >= 0) {
		const int si = pkt->stream_index;
		if (si != video_in && track_of[si] < 0) {
			av_packet_unref(pkt);
			continue;
		}
		AVRational tb = in->streams[si]->time_base;
		auto push_one = [&](AVPacket *p) {
			RingRecord rec = {};
			if (si == video_in) {
				AVRational ftb{1, fps};
				rec.pts = av_rescale_q(p->pts, tb, ftb);
				rec.dts = av_rescale_q(p->dts, tb, ftb);
				rec.tb_num = 1;
				rec.tb_den = fps;
				rec.type = 0;
				rec.keyframe = (p->flags & AV_PKT_FLAG_KEY) ? 1 : 0;
				rec.dts_usec = rec.dts * 1000000 / fps;
				rec.sys_dts_usec = video_start_sys + rec.dts_usec;
			} else {
				int sr = in->streams[si]->codecpar->sample_rate;
				AVRational atb{1, sr};
				rec.pts = av_rescale_q(p->pts, tb, atb);
				rec.dts = av_rescale_q(p->dts, tb, atb);
				rec.tb_num = 1;
				rec.tb_den = sr;
				rec.type = 1;
				rec.track = (uint8_t)track_of[si];
				rec.keyframe = 1;
				rec.dts_usec = rec.dts * 1000000 / sr;
				rec.sys_dts_usec = audio_start_sys + rec.dts_usec;
			}
			const uint8_t *data = p->data;
			size_t size = (size_t)p->size;
			std::vector<uint8_t> stripped;
			if (si == video_in && obs_headers) {
				/* Like NVENC through libobs: parameter sets only in the first packet. */
				if (!first_video_seen) {
					first_video_seen = true;
				} else {
					strip_parameter_sets(base.streams[0].codec, data, size, &stripped);
					data = stripped.data();
					size = stripped.size();
				}
				/* RingOutput keeps the headers it finds (the fix under test). */
				if (header_fix && rec.keyframe) {
					std::lock_guard lock(mu);
					std::vector<uint8_t> headers;
					if (base.streams[0].extradata.empty() &&
					    extract_codec_headers(base.streams[0].codec, data, size, &headers))
						base.streams[0].extradata = headers;
				}
			}
			rec.size = (uint32_t)size;
			ring.push(rec, data);
			pushed++;

			int64_t media_usec = rec.sys_dts_usec - video_start_sys;
			if (media_usec > newest_media_usec)
				newest_media_usec = media_usec;
			/* complete_pending(): a clip is ready once packets reach press + post */
			std::lock_guard lock(mu);
			for (auto &pr : presses)
				if (!pr.queued && newest_media_usec >= (int64_t)((pr.at + pr.post) * 1e6))
					pr.queued = true;
		};
		if (si == video_in && bsf) {
			av_bsf_send_packet(bsf, pkt);
			AVPacket *out = av_packet_alloc();
			while (av_bsf_receive_packet(bsf, out) == 0) {
				push_one(out);
				av_packet_unref(out);
			}
			av_packet_free(&out);
		} else {
			push_one(pkt);
		}
		av_packet_unref(pkt);

		/* pace */
		double media_s = newest_media_usec / 1e6;
		auto due = t0 + std::chrono::microseconds((int64_t)(media_s / speed * 1e6));
		std::this_thread::sleep_until(due);
	}
	producing = false;
	saver.join();
	av_packet_free(&pkt);
	if (bsf)
		av_bsf_free(&bsf);
	avformat_close_input(&in);
	ring.close();

	printf("pushed %llu packets, %zu streams per clip\n", (unsigned long long)pushed, base.streams.size());
	int failed = 0;
	for (auto &p : presses) {
		std::string why;
		bool ok = p.result.ok;
		if (ok) {
			/* the buffer cannot hold more than what was recorded before the press */
			double want = std::min<double>(p.pre, p.at) + p.post;
			ok = verify(p.result.path, want, base.streams.size(), &why);
		} else {
			why = p.result.error;
		}
		printf("%s  press %.0f s, last %d + %d s: %s (%.1f MB)\n", ok ? "PASS" : "FAIL", p.at, p.pre, p.post, why.c_str(),
		       p.result.bytes / 1048576.0);
		failed += ok ? 0 : 1;
	}
	printf(failed ? "%d clip(s) FAILED\n" : "all clips OK\n", failed);
	return failed ? 1 : 0;
}

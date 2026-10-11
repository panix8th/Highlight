#include "ring_output.h"

#include "../common/log.h"
#include "../common/strings.h"

#include <util/platform.h>

#include <algorithm>
#include <cmath>

namespace hl {

/* ------------------------------------------------------------------ */
/* libobs glue                                                        */

static const char *ro_get_name(void *)
{
	return "Highlight rolling buffer";
}

static void *ro_create(obs_data_t *settings, obs_output_t *)
{
	/* The RingOutput object already exists; its address travels in the settings. */
	return reinterpret_cast<RingOutput *>((uintptr_t)obs_data_get_int(settings, "instance"));
}

static void ro_destroy(void *) {}

static bool ro_start(void *data)
{
	return static_cast<RingOutput *>(data)->on_start();
}

static void ro_stop(void *data, uint64_t ts)
{
	static_cast<RingOutput *>(data)->on_stop(ts);
}

static void ro_packet(void *data, encoder_packet *pkt)
{
	static_cast<RingOutput *>(data)->on_packet(pkt);
}

static uint64_t ro_total_bytes(void *data)
{
	return static_cast<RingOutput *>(data)->disk_bytes();
}

void RingOutput::register_type()
{
	static bool done = false;
	if (done)
		return;
	done = true;

	obs_output_info info = {};
	info.id = "highlight_ring_output";
	info.flags = OBS_OUTPUT_AV | OBS_OUTPUT_ENCODED | OBS_OUTPUT_MULTI_TRACK;
	info.get_name = ro_get_name;
	info.create = ro_create;
	info.destroy = ro_destroy;
	info.start = ro_start;
	info.stop = ro_stop;
	info.encoded_packet = ro_packet;
	info.get_total_bytes = ro_total_bytes;
	info.encoded_video_codecs = "h264;hevc;av1";
	info.encoded_audio_codecs = "aac;opus";
	obs_register_output(&info);
}

/* ------------------------------------------------------------------ */

RingOutput::RingOutput()
{
	saver_ = std::thread(&RingOutput::saver_loop, this);
}

RingOutput::~RingOutput()
{
	{
		std::lock_guard lock(mutex_);
		stop_saver_ = true;
	}
	jobs_cv_.notify_all();
	if (saver_.joinable())
		saver_.join();
	if (output_) {
		obs_output_release(output_);
		output_ = nullptr;
	}
	ring_.close();
}

obs_output_t *RingOutput::create(const char *name)
{
	obs_data_t *settings = obs_data_create();
	obs_data_set_int(settings, "instance", (long long)(uintptr_t)this);
	output_ = obs_output_create("highlight_ring_output", name, settings, nullptr);
	obs_data_release(settings);
	return output_;
}

bool RingOutput::open_ring(const std::wstring &dir, int64_t keep_usec, std::string *err)
{
	return ring_.open(dir, keep_usec, err);
}

void RingOutput::close_ring()
{
	ring_.close();
}

void RingOutput::set_track_titles(std::vector<std::string> titles)
{
	std::lock_guard lock(mutex_);
	track_titles_ = std::move(titles);
}

void RingOutput::set_done_callback(DoneFn fn)
{
	std::lock_guard lock(mutex_);
	done_ = std::move(fn);
}

int64_t RingOutput::buffered_usec()
{
	int64_t oldest = ring_.oldest_sys_usec();
	int64_t newest = ring_.newest_sys_usec();
	return (oldest && newest > oldest) ? newest - oldest : 0;
}

static std::string codec_of(obs_encoder_t *enc)
{
	const char *c = obs_encoder_get_codec(enc);
	return c ? c : "";
}

bool RingOutput::on_start()
{
	if (!obs_output_can_begin_data_capture(output_, 0))
		return false;
	if (!obs_output_initialize_encoders(output_, 0))
		return false;

	std::vector<media::StreamDesc> streams;
	std::vector<int> track_to_stream(MAX_AUDIO_MIXES, -1);

	obs_encoder_t *venc = obs_output_get_video_encoder(output_);
	if (!venc)
		return false;

	media::StreamDesc v;
	v.video = true;
	v.codec = codec_of(venc);
	v.width = (int)obs_encoder_get_width(venc);
	v.height = (int)obs_encoder_get_height(venc);
	v.hdr = hdr_;
	obs_video_info ovi = {};
	if (obs_get_video_info(&ovi)) {
		v.fps_num = (int)ovi.fps_num;
		v.fps_den = (int)ovi.fps_den;
	}
	uint8_t *extra = nullptr;
	size_t extra_size = 0;
	if (obs_encoder_get_extra_data(venc, &extra, &extra_size) && extra_size)
		v.extradata.assign(extra, extra + extra_size);
	obs_data_t *vs = obs_encoder_get_settings(venc);
	v.bitrate = obs_data_get_int(vs, "bitrate") * 1000;
	obs_data_release(vs);
	streams.push_back(v);

	std::vector<std::string> titles;
	{
		std::lock_guard lock(mutex_);
		titles = track_titles_;
	}

	for (size_t i = 0; i < MAX_AUDIO_MIXES; i++) {
		obs_encoder_t *aenc = obs_output_get_audio_encoder(output_, i);
		if (!aenc)
			continue;
		media::StreamDesc a;
		a.video = false;
		a.codec = codec_of(aenc);
		a.sample_rate = (int)obs_encoder_get_sample_rate(aenc);
		a.frame_size = (int)obs_encoder_get_frame_size(aenc);
		a.channels = 2;
		if (obs_encoder_get_extra_data(aenc, &extra, &extra_size) && extra_size)
			a.extradata.assign(extra, extra + extra_size);
		obs_data_t *as = obs_encoder_get_settings(aenc);
		a.bitrate = obs_data_get_int(as, "bitrate") * 1000;
		obs_data_release(as);
		a.title = i < titles.size() ? titles[i] : ("Track " + std::to_string(i + 1));
		track_to_stream[i] = (int)streams.size();
		streams.push_back(a);
	}

	{
		std::lock_guard lock(mutex_);
		streams_ = std::move(streams);
		track_to_stream_ = std::move(track_to_stream);
		newest_sys_ = 0;
	}

	active_ = true;
	obs_output_begin_data_capture(output_, 0);
	HL_INFO("Rolling buffer started: %s %dx%d, %zu audio track(s)", v.codec.c_str(), v.width, v.height,
		streams_.size() - 1);
	return true;
}

void RingOutput::on_stop(uint64_t)
{
	if (!active_)
		return;
	active_ = false;
	obs_output_end_data_capture(output_);
	/* Write whatever we have for clips still waiting on their post-roll. */
	complete_pending(true);
	HL_INFO("Rolling buffer stopped");
}

void RingOutput::on_packet(encoder_packet *pkt)
{
	if (!active_)
		return;

	if (!pkt) {
		HL_ERROR("Encoder error, stopping the rolling buffer");
		active_ = false;
		obs_output_signal_stop(output_, OBS_OUTPUT_ENCODE_ERROR);
		return;
	}

	RingRecord rec = {};
	rec.size = (uint32_t)pkt->size;
	rec.pts = pkt->pts;
	rec.dts = pkt->dts;
	rec.dts_usec = pkt->dts_usec;
	rec.sys_dts_usec = pkt->sys_dts_usec;
	rec.tb_num = pkt->timebase_num;
	rec.tb_den = pkt->timebase_den;
	rec.type = pkt->type == OBS_ENCODER_VIDEO ? 0 : 1;
	rec.track = (uint8_t)pkt->track_idx;
	rec.keyframe = pkt->keyframe ? 1 : 0;
	ring_.push(rec, pkt->data);

	{
		std::lock_guard lock(mutex_);
		if (pkt->sys_dts_usec > newest_sys_)
			newest_sys_ = pkt->sys_dts_usec;
	}
	complete_pending(false);
}

void RingOutput::request_clip(const ClipRequest &req)
{
	{
		std::lock_guard lock(mutex_);
		pending_.push_back(req);
	}
	if (!active_)
		complete_pending(true);
}

void RingOutput::add_bookmark(int64_t sys_usec)
{
	std::lock_guard lock(mutex_);
	bookmarks_.push_back(sys_usec);
	while (bookmarks_.size() > 512)
		bookmarks_.pop_front();
}

void RingOutput::complete_pending(bool all)
{
	std::lock_guard lock(mutex_);
	bool queued = false;
	for (auto it = pending_.begin(); it != pending_.end();) {
		int64_t ready_at = it->press_sys_usec + (int64_t)it->post_seconds * 1000000;
		if (all || newest_sys_ >= ready_at) {
			Job job;
			job.req = *it;
			job.streams = streams_;
			job.track_to_stream = track_to_stream_;
			int64_t from = it->press_sys_usec - (int64_t)it->pre_seconds * 1000000 - 2000000;
			for (int64_t b : bookmarks_)
				if (b >= from && b <= ready_at)
					job.bookmarks.push_back(b);
			jobs_.push_back(std::move(job));
			it = pending_.erase(it);
			queued = true;
		} else {
			++it;
		}
	}
	if (queued)
		jobs_cv_.notify_one();
}

void RingOutput::saver_loop()
{
	for (;;) {
		Job job;
		{
			std::unique_lock lock(mutex_);
			jobs_cv_.wait(lock, [&] { return stop_saver_ || !jobs_.empty(); });
			if (stop_saver_)
				return;
			job = std::move(jobs_.front());
			jobs_.pop_front();
			saving_ = true;
		}

		ClipResult result = save(job);
		if (result.ok)
			HL_INFO("Saved clip %s (%.1f s, %s)", result.path.c_str(), result.duration,
				format_bytes(result.bytes).c_str());
		else
			HL_ERROR("Clip failed: %s", result.error.c_str());

		DoneFn done;
		{
			std::lock_guard lock(mutex_);
			done = done_;
			saving_ = false;
		}
		idle_cv_.notify_all();
		if (done)
			done(result);
	}
}

void RingOutput::wait_for_saves(int timeout_ms)
{
	std::unique_lock lock(mutex_);
	idle_cv_.wait_for(lock, std::chrono::milliseconds(timeout_ms),
			  [&] { return stop_saver_ || (jobs_.empty() && !saving_); });
}

ClipResult RingOutput::save(const Job &job)
{
	ClipResult res;
	res.id = job.req.id;
	res.path = job.req.path;
	res.game = job.req.game;

	if (job.streams.empty()) {
		res.error = "The buffer is not running";
		return res;
	}

	const int64_t from = job.req.press_sys_usec - (int64_t)job.req.pre_seconds * 1000000;
	const int64_t to = job.req.press_sys_usec + (int64_t)job.req.post_seconds * 1000000;

	media::Mp4Writer writer;
	bool opened = false;
	bool failed = false;
	std::string err;

	int64_t k_pts = 0;
	int64_t video_start_usec = 0;
	int64_t video_start_sys = 0;
	int64_t last_video_end = 0;
	int32_t video_den = 1;

	struct TrackState {
		bool started = false;
		int64_t first_dts = 0;
		int64_t base = 0;
	};
	std::vector<TrackState> tracks(MAX_AUDIO_MIXES);

	auto fn = [&](const RingRecord &h, const uint8_t *data) -> bool {
		if (!opened) {
			if (!(h.type == 0 && h.keyframe))
				return true;
			k_pts = h.pts;
			video_den = h.tb_den;
			int64_t pts_offset_usec = (h.pts - h.dts) * 1000000 / std::max(1, h.tb_den);
			video_start_usec = h.dts_usec + pts_offset_usec;
			video_start_sys = h.sys_dts_usec + pts_offset_usec;

			std::vector<media::Chapter> chapters;
			double clip_len = (double)(to - video_start_sys) / 1e6;
			std::vector<double> marks;
			for (int64_t b : job.bookmarks) {
				double t = (double)(b - video_start_sys) / 1e6;
				if (t > 0.05 && t < clip_len)
					marks.push_back(t);
			}
			std::sort(marks.begin(), marks.end());
			if (!marks.empty()) {
				chapters.push_back({0.0, marks[0], "Start"});
				for (size_t i = 0; i < marks.size(); i++) {
					double end = i + 1 < marks.size() ? marks[i + 1] : clip_len;
					chapters.push_back({marks[i], end, "Bookmark " + std::to_string(i + 1)});
				}
			}
			res.bookmarks = (int)marks.size();

			if (!writer.open(job.req.path, job.req.format, job.streams, chapters, &err)) {
				failed = true;
				return false;
			}
			opened = true;
		}

		if (h.type == 0) {
			int64_t pts = h.pts - k_pts;
			int64_t dts = h.dts - k_pts;
			if (!writer.write(0, data, h.size, pts, dts, h.tb_den, h.keyframe != 0)) {
				failed = true;
				err = "Could not write video";
				return false;
			}
			int64_t frame = std::max<int64_t>(1, h.tb_num);
			last_video_end = std::max(last_video_end, pts + frame);
		} else {
			int stream = h.track < job.track_to_stream.size() ? job.track_to_stream[h.track] : -1;
			if (stream < 0)
				return true;
			TrackState &t = tracks[h.track];
			if (!t.started) {
				int64_t rel_usec = h.dts_usec - video_start_usec;
				if (rel_usec < 0)
					return true;
				t.started = true;
				t.first_dts = h.dts;
				t.base = (int64_t)llround((double)rel_usec * h.tb_den / 1e6);
			}
			int64_t ts = h.dts - t.first_dts + t.base;
			if (!writer.write(stream, data, h.size, ts, ts, h.tb_den, true)) {
				failed = true;
				err = "Could not write audio";
				return false;
			}
		}
		return true;
	};

	if (!ring_.read_range(from, to, fn, &err) || failed || !opened) {
		writer.abort();
		res.error = err.empty() ? "Nothing to save yet" : err;
		return res;
	}

	if (!writer.finish(&err)) {
		res.error = err;
		return res;
	}

	res.ok = true;
	res.duration = (double)last_video_end / std::max(1, video_den);
	res.bytes = writer.bytes_written();
	return res;
}

} // namespace hl

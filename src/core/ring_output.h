#pragma once

/* A custom libobs output that receives encoded packets from the video and
 * audio encoders, appends them to the DiskRing, and turns time ranges of the
 * ring into clip files on request (with post-roll). */

#include "disk_ring.h"

#include "../media/mp4_writer.h"

#include <obs.h>

#include <atomic>
#include <condition_variable>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace hl {

struct ClipRequest {
	uint64_t id = 0;
	int64_t press_sys_usec = 0;
	int pre_seconds = 60;
	int post_seconds = 0;
	std::string path;   /* final file path, UTF-8 */
	std::string format; /* "mp4" | "mov" | "matroska" */
	std::string game;
};

struct ClipResult {
	uint64_t id = 0;
	bool ok = false;
	std::string path;
	std::string game;
	std::string error;
	double duration = 0;
	uint64_t bytes = 0;
	int bookmarks = 0;
};

class RingOutput {
public:
	using DoneFn = std::function<void(const ClipResult &)>;

	static void register_type();

	RingOutput();
	~RingOutput();
	RingOutput(const RingOutput &) = delete;
	RingOutput &operator=(const RingOutput &) = delete;

	/* Creates the libobs output object bound to this instance. */
	obs_output_t *create(const char *name);
	obs_output_t *output() const { return output_; }

	bool open_ring(const std::wstring &dir, int64_t keep_usec, std::string *err);
	void close_ring();
	void set_keep(int64_t keep_usec) { ring_.set_keep(keep_usec); }

	void set_track_titles(std::vector<std::string> titles);
	void set_hdr(bool hdr) { hdr_ = hdr; }
	void set_done_callback(DoneFn fn);

	/* Queues a clip. It is written once the post-roll has been captured. */
	void request_clip(const ClipRequest &req);
	void add_bookmark(int64_t sys_usec);
	/* Waits until queued clips have been written (used before closing the ring). */
	void wait_for_saves(int timeout_ms);

	bool active() const { return active_; }
	uint64_t disk_bytes() const { return ring_.disk_bytes(); }
	int64_t buffered_usec();

	/* libobs callbacks */
	bool on_start();
	void on_stop(uint64_t ts);
	void on_packet(encoder_packet *pkt);

private:
	struct Job {
		ClipRequest req;
		std::vector<media::StreamDesc> streams;
		std::vector<int> track_to_stream;
		std::vector<int64_t> bookmarks;
	};

	void saver_loop();
	ClipResult save(const Job &job);
	void complete_pending(bool all);

	obs_output_t *output_ = nullptr;
	DiskRing ring_;
	std::atomic<bool> active_{false};
	bool hdr_ = false;

	std::mutex mutex_;
	std::vector<media::StreamDesc> streams_; /* [0] video, [1..] audio */
	std::vector<int> track_to_stream_;       /* encoder track index -> stream index */
	std::vector<std::string> track_titles_;
	std::deque<ClipRequest> pending_;
	std::deque<int64_t> bookmarks_;
	int64_t newest_sys_ = 0;

	std::thread saver_;
	std::condition_variable jobs_cv_;
	std::deque<Job> jobs_;
	std::condition_variable idle_cv_;
	bool saving_ = false;
	bool stop_saver_ = false;
	DoneFn done_;
};

} // namespace hl

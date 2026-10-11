#pragma once

/* Everything that talks to libobs: startup, capture sources, audio tracks,
 * encoders, the rolling buffer output, full-session recording and screenshots. */

#include "ring_output.h"

#include "../common/settings.h"

#include <obs.h>

#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace hl {

class Engine {
public:
	struct Events {
		std::function<void(const std::string &game)> game_hooked; /* any thread */
		std::function<void()> game_unhooked;
		std::function<void(const ClipResult &)> clip_done;
		std::function<void(bool ok, const std::string &path)> screenshot_done;
		std::function<void(bool recording, const std::string &path, const std::string &error)> recording_changed;
		std::function<void(const std::string &message)> error;
	};

	Engine();
	~Engine();

	bool init(const Settings &settings, Events events, std::string *err);
	void shutdown();

	/* Rebuilds whatever the new settings require. Restarts the buffer if it was running. */
	bool apply(const Settings &settings, std::string *err, bool force = false);

	bool start_buffer(std::string *err);
	void stop_buffer();
	bool buffer_active() const;

	/* Saves the last `pre` seconds plus `post` seconds after now. Returns the clip id. */
	uint64_t save_clip(int pre, int post);
	void bookmark();
	void screenshot();
	bool toggle_recording(std::string *err);
	bool recording() const { return recording_active_; }
	int64_t recording_seconds() const;

	void set_push_to_talk(bool down);

	std::string game() const;
	std::string encoder_label() const { return encoder_label_; }
	int64_t buffered_usec() { return ring_ ? ring_->buffered_usec() : 0; }
	uint64_t ring_bytes() const { return ring_ ? ring_->disk_bytes() : 0; }
	int output_width() const { return out_w_; }
	int output_height() const { return out_h_; }
	int fps() const { return settings_.video.fps; }

	/* Called from the libobs graphics thread. */
	void on_rendered();
	void on_hooked(const char *title, const char *cls, const char *exe);
	void on_unhooked();
	void on_recording_stopped(int code);

private:
	bool reset_audio();
	bool reset_video(std::string *err);
	bool build_capture(std::string *err);
	void destroy_capture();
	bool build_audio();
	void destroy_audio();
	bool build_encoders(std::string *err);
	void destroy_encoders();
	bool build_outputs(std::string *err);
	void destroy_outputs();
	void apply_mic_state();
	std::string make_path(const std::string &suffix, const std::string &ext) const;
	std::string container_format() const;
	std::string container_ext() const;

	Settings settings_;
	Events events_;
	bool started_ = false;

	obs_scene_t *scene_ = nullptr;
	obs_source_t *capture_ = nullptr;
	std::vector<obs_source_t *> audio_sources_; /* channel 1.. */
	std::vector<int> audio_track_index_;        /* settings track index per audio source */
	obs_source_t *mic_ = nullptr;
	obs_source_t *game_audio_ = nullptr;

	obs_encoder_t *venc_ = nullptr;
	std::vector<obs_encoder_t *> aencs_;
	std::vector<std::string> track_titles_;
	std::string encoder_label_;

	std::unique_ptr<RingOutput> ring_;
	obs_output_t *record_ = nullptr;
	std::string record_path_;
	std::atomic<bool> recording_active_{false};
	int64_t record_started_ns_ = 0;

	int base_w_ = 1920, base_h_ = 1080;
	int out_w_ = 1920, out_h_ = 1080;

	mutable std::mutex game_mutex_;
	std::string game_;
	std::string game_window_spec_;

	std::atomic<bool> ptt_down_{false};
	std::atomic<uint64_t> next_clip_id_{1};

	/* screenshot state (graphics thread) */
	std::atomic<bool> screenshot_requested_{false};
	void *stage_ = nullptr;
	int stage_frames_ = 0;
	uint32_t stage_w_ = 0, stage_h_ = 0;
	bool stage_rgba_ = false;
	bool rendered_cb_ = false;
	std::string pending_screenshot_path_;
	std::thread shot_thread_;
};

} // namespace hl

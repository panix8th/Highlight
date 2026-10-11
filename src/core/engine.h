#pragma once

/* Everything that talks to libobs: startup, capture sources, audio tracks,
 * encoders, the rolling buffer output, full-session recording and screenshots. */

#include "ring_output.h"

#include "../common/settings.h"

#include <windows.h>

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

	/* "Capture the game in front": retargets game capture to this window and falls
	 * back to window capture (Windows Graphics Capture) if the hook does not take.
	 * Stays on that window until it closes, then goes back to automatic detection. */
	bool capture_window(HWND hwnd, std::string *name, std::string *exe, std::string *err);
	void capture_auto();
	void tick(); /* once a second, on the app thread: fallback and window-closed checks */
	bool forced() const { return forced_hwnd_ != nullptr; }
	HWND forced_window() const { return forced_hwnd_; }
	bool game_hooked() const { return game_hooked_; }
	/* OBS game capture never hooks these executables (browsers, Discord, the shell) */
	static bool game_capture_refuses(const std::string &exe);
	/* title, class and executable file name of a window (UTF-8); false if unreadable */
	static bool window_info(HWND hwnd, std::string *title, std::string *cls, std::string *exe);
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
	void load_modules();
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
	bool modules_loaded_ = false;

	obs_scene_t *scene_ = nullptr;
	obs_source_t *capture_ = nullptr;
	obs_sceneitem_t *capture_item_ = nullptr;

	/* forced capture (app thread only) */
	HWND forced_hwnd_ = nullptr;
	std::string forced_spec_;
	std::string forced_name_;
	uint64_t forced_since_ms_ = 0;
	int forced_priority_ = 2; /* libobs window_priority used to find the window */
	int forced_hidden_ticks_ = 0;
	obs_source_t *fallback_ = nullptr; /* window_capture used when the hook fails */
	obs_sceneitem_t *fallback_item_ = nullptr;
	/* empty spec: any fullscreen game; idle: stop trying to hook (the fallback captures) */
	void set_game_capture_target(const std::string &spec, bool idle = false);
	obs_sceneitem_t *add_to_scene(obs_source_t *src);
	void start_fallback();
	/* notify: report the game as gone when nothing else captures it */
	void stop_fallback(bool notify = true);
	void target_game_audio(const std::string &spec);
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
	std::atomic<bool> game_hooked_{false};
	std::atomic<bool> fallback_on_{false}; /* read by on_unhooked (graphics thread) */

	void save_screen_grab(const std::string &path); /* fallback when the canvas has no picture */
};

} // namespace hl

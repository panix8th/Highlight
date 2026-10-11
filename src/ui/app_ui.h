#pragma once

/* The Highlight window: library, editor, settings and the share dialog.
 * Runs in its own process so closing it frees its memory; the background
 * core keeps capturing. */

#include "audio_out.h"
#include "textures.h"
#include "widgets.h"

#include "../common/ipc.h"
#include "../common/library.h"
#include "../common/settings.h"
#include "../media/player.h"
#include "../media/probe.h"
#include "../media/share.h"

#include <windows.h>

#include <atomic>
#include <chrono>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace ui {

enum class Page { Library, Editor, Clipping, Video, Audio, Overlay };

struct ClipItem {
	std::string path;
	std::string name;
	std::string game;
	uint64_t size = 0;
	int64_t mtime = 0; /* unix seconds */
	uint64_t stamp = 0;
	double duration = -1;
	int height = 0;
	double fps = 0;
	int bookmarks = 0;
	bool session = false;
	bool screenshot = false;
};

struct CoreStatus {
	bool connected = false;
	std::string buffer = "idle";
	std::string game;
	bool game_running = false;
	double buffered = 0;
	double ram_mb = 0;
	double ring_mb = 0;
	bool recording = false;
	int64_t record_seconds = 0;
	std::string encoder;
	int height = 0;
	int fps = 0;
	int clip_seconds = 60;
	std::string error;
};

struct Job {
	std::thread thread;
	std::atomic<bool> running{false};
	std::atomic<bool> cancel{false};
	std::atomic<float> progress{0};
	std::mutex mutex;
	bool done = false;
	bool ok = false;
	std::string result; /* output path */
	std::string error;
	int64_t bytes = 0;

	void join()
	{
		if (thread.joinable())
			thread.join();
	}
	~Job() { join(); }
};

class AppUI {
public:
	bool init(HWND hwnd);
	void shutdown();
	void frame();

	bool wants_fast_refresh() const;
	bool hit_caption(POINT client_pt) const;
	bool should_close() const { return close_requested_; }
	void request_show();

private:
	/* app_ui.cpp */
	void poll_core();
	void try_connect();
	void send(const hl::ipc::json &msg);
	void draw_titlebar();
	void draw_sidebar();
	void draw_banner();
	void page_header(const char *title, const char *subtitle);
	void mark_settings_dirty();
	void flush_settings(bool force);
	void scan_library_async();
	void show_in_folder(const std::string &path);
	void delete_clip(const std::string &path);
	void open_editor(const ClipItem &clip);
	void notice(const std::string &text, bool error = false);
	void draw_notice();

	/* library_page.cpp */
	void library_page();
	void clip_card(const ClipItem &c, float w);
	void clip_menu();

	/* editor_page.cpp */
	void editor_page();
	void editor_close();
	void editor_timeline(float width);
	void editor_export_panel(float width);
	void editor_keyboard();
	std::vector<int> editor_selected_streams(bool for_lossless) const;
	void start_trim_job(bool replace);

	/* settings_pages.cpp */
	void clipping_page();
	void video_page();
	void audio_page();
	void overlay_page();
	void hotkey_row(const char *id, const char *label, const char *desc, hl::Hotkey &key, bool accent = false);
	void poll_hotkey_capture();

	/* share_dialog.cpp */
	void open_share();
	void share_dialog();

	HWND hwnd_ = nullptr;
	Page page_ = Page::Library;
	bool close_requested_ = false;
	bool show_requested_ = false;
	std::chrono::steady_clock::time_point last_fast_ = {};

	hl::ipc::Client core_;
	CoreStatus status_;
	double next_connect_ = 0;
	bool core_launch_tried_ = false;

	hl::Settings settings_;
	bool settings_dirty_ = false;
	double settings_dirty_at_ = 0;
	hl::Library library_;

	/* library */
	std::mutex clips_mutex_;
	std::vector<ClipItem> clips_;
	std::atomic<bool> scanning_{false};
	std::atomic<bool> rescan_{false};
	std::thread scan_thread_;
	uint64_t clips_total_bytes_ = 0;
	ThumbCache thumbs_;
	std::string filter_ = "All";
	char search_[128] = {};
	int sort_ = 0;
	std::string menu_path_;
	bool menu_open_request_ = false;
	std::string rename_path_;
	char rename_buf_[256] = {};
	bool open_rename_ = false;

	/* editor */
	struct Editor {
		bool open = false;
		ClipItem clip;
		hl::media::MediaInfo info;
		hl::media::Player player;
		AudioOut audio;
		Texture frame;
		std::vector<uint8_t> px;
		double in = 0;
		double out = 0;
		int mix_stream = -1;         /* stream index of the "Mix" track, if any */
		std::vector<int> lanes;      /* audio stream indices shown as lanes */
		std::vector<std::string> lane_names;
		std::vector<bool> lane_on;
		std::mutex extra_mutex;
		std::vector<std::vector<float>> peaks; /* per lane */
		std::vector<Texture> strip;
		std::vector<std::vector<uint8_t>> strip_px;
		std::vector<int> strip_wh;
		std::thread loader;
		std::atomic<bool> loader_stop{false};
		int cut_mode = 0; /* 0 lossless, 1 frame-exact */
		int format = 0;   /* 0 mp4, 1 mkv */
		bool confirm_replace = false;
	};
	std::unique_ptr<Editor> ed_;
	Job trim_job_;

	/* share */
	bool share_open_ = false;
	bool share_popup_pending_ = false;
	Job share_job_;

	/* settings pages */
	hl::Hotkey *capturing_ = nullptr;
	double capture_started_ = 0;
	std::vector<std::pair<std::string, std::string>> out_devices_, in_devices_;
	bool devices_loaded_ = false;

	/* notices */
	std::string notice_;
	bool notice_error_ = false;
	double notice_until_ = 0;
};

double now_seconds();

} // namespace ui

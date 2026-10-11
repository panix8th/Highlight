#pragma once

/* The background process: tray icon, hotkeys, in-game toasts, the IPC server
 * the UI talks to, and the policy for when the rolling buffer runs. */

#include "engine.h"
#include "hotkeys.h"
#include "toast.h"

#include "../common/ipc.h"
#include "../common/settings.h"

#include <windows.h>

#include <atomic>
#include <functional>
#include <map>
#include <string>

namespace hl {

class App {
public:
	int run(HINSTANCE inst, bool open_ui);

private:
	static LRESULT CALLBACK wndproc(HWND, UINT, WPARAM, LPARAM);
	LRESULT handle(UINT msg, WPARAM wp, LPARAM lp);

	void post(std::function<void()> fn);
	void apply_hotkeys();
	void on_hotkey(int id, bool pressed);
	void save(int pre, const std::string &key);
	void toggle_pause();
	void toggle_recording();
	void update_buffer_state();
	void handle_ipc(const ipc::json &msg);
	void apply_settings(const Settings &s);
	void on_clip_done(const ClipResult &r);

	void add_tray();
	void update_tray();
	void show_tray_menu();
	void launch_ui();
	void open_clips_folder();
	void set_autostart(bool on);
	void cleanup_async();

	ipc::json status_json();
	void broadcast_status();
	/* "Capture the game in front" (hotkey, tray, or a remembered game coming to the front) */
	enum class CaptureReason { Hotkey, Remembered, KnownGame };
	void capture_window(HWND hwnd, CaptureReason why);
	static bool capture_candidate(HWND hwnd);
	void watch_foreground(); /* once a second: remember the last game-like window, pick up remembered games */
	void forget_game(const std::string &exe);
	void toast(ToastKind kind, const std::string &title, const std::string &line1 = "",
		   const std::string &line2 = "", const std::string &key = "");

	HINSTANCE inst_ = nullptr;
	HWND hwnd_ = nullptr;
	UINT taskbar_created_ = 0;
	ULONG_PTR gdiplus_token_ = 0;
	HICON icons_[5] = {};
	int tray_state_ = -1;
	ULONGLONG saved_flash_until_ = 0;

	Settings settings_;
	Engine engine_;
	Hotkeys hotkeys_;
	Overlay overlay_;
	ipc::Server ipc_;

	bool engine_ok_ = false;
	std::string engine_error_;
	bool paused_ = false;
	bool game_running_ = false;
	bool ever_hooked_ = false;
	uint64_t idle_grace_until_ = 0; /* GetTickCount64: keep the buffer through brief unhooks */
	HWND last_candidate_ = nullptr;     /* last game-like window in front (for the tray menu) */
	HWND last_auto_try_ = nullptr;      /* game window we already tried to capture by itself */
	HWND fg_window_ = nullptr;          /* window in front, and for how many seconds */
	int fg_seconds_ = 0;
	std::map<uint64_t, std::string> pending_keys_;
	std::atomic<bool> cleaning_{false};
};

} // namespace hl

#include "app.h"
#include "gdiplus_include.h"
#include "icons.h"
#include "sound.h"

#include "../common/games.h"
#include "../common/library.h"
#include "../common/log.h"
#include "../common/paths.h"
#include "../common/strings.h"

#include <psapi.h>
#include <dwmapi.h>
#include <shellapi.h>

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <thread>
#include <vector>

namespace hl {

namespace fs = std::filesystem;

enum : UINT {
	WM_APP_TRAY = WM_APP + 1,
	WM_APP_HOTKEY = WM_APP + 2,
	WM_APP_CALL = WM_APP + 3,
};

enum : UINT_PTR { TIMER_STATUS = 1, TIMER_CLEANUP = 2, TIMER_TEST_TOAST = 3, TIMER_IDLE_STOP = 4 };

/* Games drop the capture hook on alt-tab, loading screens and mode switches.
 * Keep recording that long before treating the game as closed. */
static constexpr UINT k_idle_grace_ms = 90 * 1000;

enum : int {
	HK_SAVE = 1,
	HK_LONG,
	HK_QUICK,
	HK_BOOKMARK,
	HK_SHOT,
	HK_RECORD,
	HK_PAUSE,
	HK_PTT,
	HK_CAPTURE,
	HK_EXTRA = 100,
};

enum : UINT {
	CMD_SAVE = 1001,
	CMD_SAVE_LONG,
	CMD_RECORD,
	CMD_PAUSE,
	CMD_FOLDER,
	CMD_OPEN,
	CMD_QUIT,
	CMD_CAPTURE,
};

static const wchar_t *k_window_class = L"HighlightCore";

/* 65 -> "65 seconds", 300 -> "5:00" */
static std::string span_text(double seconds)
{
	if (seconds < 119.5)
		return std::to_string((int)std::lround(seconds)) + " seconds";
	return format_duration(seconds);
}

/* ------------------------------------------------------------------ */

int App::run(HINSTANCE inst, bool open_ui)
{
	inst_ = inst;

	Gdiplus::GdiplusStartupInput gsi;
	Gdiplus::GdiplusStartup(&gdiplus_token_, &gsi, nullptr);

	if (!fs::exists(paths::settings_file())) {
		settings_ = Settings::defaults();
		settings_.save();
	} else {
		settings_ = Settings::load();
	}

	WNDCLASSEXW wc = {sizeof(wc)};
	wc.lpfnWndProc = wndproc;
	wc.hInstance = inst;
	wc.lpszClassName = k_window_class;
	RegisterClassExW(&wc);
	hwnd_ = CreateWindowExW(WS_EX_TOOLWINDOW, k_window_class, L"Highlight", WS_POPUP, 0, 0, 0, 0, nullptr, nullptr,
				inst, this);
	if (!hwnd_)
		return 1;
	taskbar_created_ = RegisterWindowMessageW(L"TaskbarCreated");

	overlay_.init(inst);

	const int icon_size = GetSystemMetrics(SM_CXSMICON);
	icons_[0] = make_icon(IconState::Idle, icon_size);
	icons_[1] = make_icon(IconState::Live, icon_size);
	icons_[2] = make_icon(IconState::Saved, icon_size);
	icons_[3] = make_icon(IconState::Paused, icon_size);
	icons_[4] = make_icon(IconState::Recording, icon_size);
	add_tray();

	Engine::Events ev;
	ev.game_hooked = [this](const std::string &) {
		post([this]() {
			game_running_ = true;
			ever_hooked_ = true;
			idle_grace_until_ = 0;
			KillTimer(hwnd_, TIMER_IDLE_STOP);
			update_buffer_state();
			broadcast_status();
		});
	};
	ev.game_unhooked = [this]() {
		post([this]() {
			game_running_ = false;
			idle_grace_until_ = GetTickCount64() + k_idle_grace_ms;
			SetTimer(hwnd_, TIMER_IDLE_STOP, k_idle_grace_ms + 500, nullptr);
			update_buffer_state();
			broadcast_status();
		});
	};
	ev.clip_done = [this](const ClipResult &r) { post([this, r]() { on_clip_done(r); }); };
	ev.screenshot_done = [this](bool ok, const std::string &path) {
		post([this, ok, path]() {
			if (ok) {
				const size_t slash = path.find_last_of("\\/");
				toast(ToastKind::Saved, "Screenshot saved", "PNG · full resolution",
				      slash == std::string::npos ? path : path.substr(slash + 1),
				      settings_.hotkeys.screenshot.to_string());
				if (settings_.overlay.sound)
					play_cue(Cue::Bookmark, settings_.overlay.sound_volume);
				ipc_.broadcast({{"evt", "screenshot_saved"}, {"path", path}});
			} else {
				toast(ToastKind::Error, "Screenshot failed");
			}
		});
	};
	ev.recording_changed = [this](bool on, const std::string &path, const std::string &error) {
		post([this, on, path, error]() {
			if (!error.empty())
				toast(ToastKind::Error, "Recording stopped", error);
			else if (!on)
				toast(ToastKind::Saved, "Recording saved", "Full session", path);
			if (settings_.overlay.sound)
				play_cue(on ? Cue::RecordStart : Cue::RecordStop, settings_.overlay.sound_volume);
			overlay_.set_recording(on && settings_.overlay.rec_indicator, 0);
			if (!on)
				ipc_.broadcast({{"evt", "clip_saved"}, {"path", path}, {"recording", true}});
			update_tray();
			broadcast_status();
		});
	};
	ev.error = [this](const std::string &msg) { post([this, msg]() { toast(ToastKind::Error, "Highlight", msg); }); };

	/* Accept UI connections right away; requests are handled once the message loop runs. */
	ipc_.start([this](const ipc::json &msg, ipc::Connection &) {
		std::string text = msg.dump();
		post([this, text]() {
			try {
				handle_ipc(ipc::json::parse(text));
			} catch (...) {
			}
		});
	});

	std::string err;
	engine_ok_ = engine_.init(settings_, ev, &err);
	if (!engine_ok_) {
		engine_error_ = err;
		HL_ERROR("Engine failed to start: %s", err.c_str());
		toast(ToastKind::Error, "Highlight could not start capturing", err);
	}

	apply_hotkeys();
	hotkeys_.start([this](int id, bool pressed) { PostMessageW(hwnd_, WM_APP_HOTKEY, (WPARAM)id, pressed ? 1 : 0); });

	update_buffer_state();
	SetTimer(hwnd_, TIMER_STATUS, 1000, nullptr);
	SetTimer(hwnd_, TIMER_CLEANUP, 60 * 60 * 1000, nullptr);
	cleanup_async();
	set_autostart(settings_.clipping.start_with_windows);

	if (open_ui)
		launch_ui();

	MSG msg;
	while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
		TranslateMessage(&msg);
		DispatchMessageW(&msg);
	}

	HL_INFO("Shutting down");
	KillTimer(hwnd_, TIMER_STATUS);
	KillTimer(hwnd_, TIMER_CLEANUP);
	hotkeys_.stop();
	ipc_.broadcast({{"evt", "core_exit"}});
	ipc_.stop();
	engine_.shutdown();

	NOTIFYICONDATAW nid = {sizeof(nid)};
	nid.hWnd = hwnd_;
	nid.uID = 1;
	Shell_NotifyIconW(NIM_DELETE, &nid);
	overlay_.shutdown();
	for (HICON &i : icons_)
		if (i)
			DestroyIcon(i);
	DestroyWindow(hwnd_);
	while (cleaning_)
		Sleep(50);
	Gdiplus::GdiplusShutdown(gdiplus_token_);
	return 0;
}

LRESULT CALLBACK App::wndproc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
	if (msg == WM_NCCREATE) {
		auto *cs = (CREATESTRUCTW *)lp;
		SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR)cs->lpCreateParams);
	}
	auto *self = (App *)GetWindowLongPtrW(hwnd, GWLP_USERDATA);
	if (self && self->hwnd_ == hwnd)
		return self->handle(msg, wp, lp);
	return DefWindowProcW(hwnd, msg, wp, lp);
}

LRESULT App::handle(UINT msg, WPARAM wp, LPARAM lp)
{
	switch (msg) {
	case WM_APP_CALL: {
		auto *fn = (std::function<void()> *)lp;
		(*fn)();
		delete fn;
		return 0;
	}
	case WM_APP_HOTKEY:
		on_hotkey((int)wp, lp != 0);
		return 0;
	case WM_APP_TRAY:
		switch (LOWORD(lp)) {
		case WM_CONTEXTMENU:
		case WM_RBUTTONUP:
			show_tray_menu();
			break;
		case NIN_SELECT:
		case NIN_KEYSELECT:
		case WM_LBUTTONDBLCLK:
			launch_ui();
			break;
		}
		return 0;
	case WM_TIMER:
		if (wp == TIMER_STATUS) {
			engine_.tick();
			watch_foreground();
			broadcast_status();
			update_tray();
			if (engine_.recording() && settings_.overlay.rec_indicator)
				overlay_.set_recording(true, engine_.recording_seconds());
		} else if (wp == TIMER_CLEANUP) {
			cleanup_async();
		} else if (wp == TIMER_IDLE_STOP) {
			KillTimer(hwnd_, TIMER_IDLE_STOP);
			update_buffer_state();
		} else if (wp == TIMER_TEST_TOAST) {
			KillTimer(hwnd_, TIMER_TEST_TOAST);
			/* Shown directly: the test started its card even with banners switched off. */
			ToastContent c;
			c.kind = ToastKind::Saved;
			c.title = "Clip saved";
			c.line1 = "Last " + span_text(settings_.clipping.clip_seconds + 2) + " · " +
				  std::to_string(engine_.output_height()) + "p" + std::to_string(engine_.fps()) + " · test";
			c.line2 = "Example_clip.mp4";
			c.key = settings_.hotkeys.save.to_string();
			if (c.key == "Not set")
				c.key.clear();
			overlay_.show_toast(c, settings_.overlay.position, settings_.overlay.toast_seconds);
		}
		return 0;
	case WM_COMMAND:
		switch (LOWORD(wp)) {
		case CMD_SAVE:
			save(settings_.clipping.clip_seconds, settings_.hotkeys.save.to_string());
			break;
		case CMD_SAVE_LONG:
			save(settings_.clipping.long_seconds, settings_.hotkeys.save_long.to_string());
			break;
		case CMD_RECORD:
			toggle_recording();
			break;
		case CMD_PAUSE:
			toggle_pause();
			break;
		case CMD_FOLDER:
			open_clips_folder();
			break;
		case CMD_OPEN:
			launch_ui();
			break;
		case CMD_CAPTURE:
			capture_window(last_candidate_, CaptureReason::Hotkey);
			break;
		case CMD_QUIT:
			PostQuitMessage(0);
			break;
		}
		return 0;
	case WM_QUERYENDSESSION:
		return TRUE;
	case WM_ENDSESSION:
		if (wp)
			PostQuitMessage(0);
		return 0;
	}
	if (msg == taskbar_created_ && taskbar_created_) {
		add_tray();
		return 0;
	}
	return DefWindowProcW(hwnd_, msg, wp, lp);
}

void App::post(std::function<void()> fn)
{
	auto *heap = new std::function<void()>(std::move(fn));
	if (!PostMessageW(hwnd_, WM_APP_CALL, 0, (LPARAM)heap))
		delete heap;
}

/* ------------------------------------------------------------------ */
/* hotkeys and actions                                                */

void App::apply_hotkeys()
{
	const auto &h = settings_.hotkeys;
	std::vector<Hotkeys::Binding> b = {
		{HK_SAVE, h.save, false},         {HK_LONG, h.save_long, false}, {HK_QUICK, h.save_quick, false},
		{HK_BOOKMARK, h.bookmark, false}, {HK_SHOT, h.screenshot, false}, {HK_RECORD, h.record, false},
		{HK_PAUSE, h.pause, false},       {HK_CAPTURE, h.capture_game, false},
	};
	if (settings_.audio.push_to_talk)
		b.push_back({HK_PTT, h.push_to_talk, true});
	for (size_t i = 0; i < h.extra.size(); i++)
		b.push_back({HK_EXTRA + (int)i, h.extra[i].key, false});
	hotkeys_.set_bindings(std::move(b));
}

void App::on_hotkey(int id, bool pressed)
{
	const auto &h = settings_.hotkeys;
	const auto &c = settings_.clipping;
	if (id == HK_PTT) {
		engine_.set_push_to_talk(pressed);
		return;
	}
	if (!pressed)
		return;

	switch (id) {
	case HK_SAVE:
		save(c.clip_seconds, h.save.to_string());
		break;
	case HK_LONG:
		save(c.long_seconds, h.save_long.to_string());
		break;
	case HK_QUICK:
		save(c.quick_seconds, h.save_quick.to_string());
		break;
	case HK_BOOKMARK:
		if (!engine_.buffer_active())
			break;
		engine_.bookmark();
		if (settings_.overlay.sound)
			play_cue(Cue::Bookmark, settings_.overlay.sound_volume);
		{
			SYSTEMTIME t;
			GetLocalTime(&t);
			char buf[32];
			snprintf(buf, sizeof(buf), "%02d:%02d:%02d", t.wHour, t.wMinute, t.wSecond);
			toast(ToastKind::Bookmark, "Bookmark added", buf, "Shows up on the clip timeline", h.bookmark.to_string());
		}
		break;
	case HK_SHOT:
		if (engine_ok_)
			engine_.screenshot();
		break;
	case HK_RECORD:
		toggle_recording();
		break;
	case HK_CAPTURE:
		capture_window(GetForegroundWindow(), CaptureReason::Hotkey);
		break;
	case HK_PAUSE:
		toggle_pause();
		break;
	default:
		if (id >= HK_EXTRA && id - HK_EXTRA < (int)h.extra.size())
			save(h.extra[id - HK_EXTRA].seconds, h.extra[id - HK_EXTRA].key.to_string());
		break;
	}
}

void App::save(int pre, const std::string &key)
{
	if (!engine_ok_ || !engine_.buffer_active()) {
		std::string why = !engine_ok_ ? engine_error_
				  : paused_   ? "The buffer is paused. Press " + settings_.hotkeys.pause.to_string() +
							  " to resume."
						      : "No game detected yet. Highlight starts recording when a game runs.";
		toast(ToastKind::Error, "Nothing to save", why);
		if (settings_.overlay.sound)
			play_cue(Cue::Error, settings_.overlay.sound_volume);
		return;
	}

	const int post_s = settings_.clipping.post_seconds;
	uint64_t id = engine_.save_clip(pre, post_s);
	pending_keys_[id] = key;
	if (settings_.overlay.sound)
		play_cue(Cue::Saved, settings_.overlay.sound_volume);
	if (settings_.overlay.toast) {
		ToastContent c;
		c.kind = ToastKind::Working;
		c.title = "Saving clip";
		c.line1 = "Last " + span_text(pre) + (post_s > 0 ? " + " + std::to_string(post_s) + " s after" : "");
		std::string game = engine_.game();
		c.line2 = post_s > 0 ? "Keep playing, still recording" : (game.empty() ? "Writing the file" : game);
		c.key = key == "Not set" ? "" : key;
		overlay_.show_progress(c, settings_.overlay.position, post_s + 1.0);
	}
}

void App::on_clip_done(const ClipResult &r)
{
	std::string key;
	if (auto it = pending_keys_.find(r.id); it != pending_keys_.end()) {
		key = it->second;
		pending_keys_.erase(it);
	}

	if (!r.ok) {
		toast(ToastKind::Error, "Clip not saved", r.error);
		if (settings_.overlay.sound)
			play_cue(Cue::Error, settings_.overlay.sound_volume);
		ipc_.broadcast({{"evt", "clip_failed"}, {"error", r.error}});
		return;
	}

	std::string spec = "Last " + span_text(r.duration) + " · " + std::to_string(engine_.output_height()) + "p" +
			   std::to_string(engine_.fps()) + " · " + format_bytes(r.bytes);
	/* The file name already carries the game and time; the folder is in the app. */
	const size_t slash = r.path.find_last_of("\\/");
	toast(ToastKind::Saved, "Clip saved", spec, slash == std::string::npos ? r.path : r.path.substr(slash + 1), key);
	saved_flash_until_ = GetTickCount64() + 2000;
	update_tray();
	ipc_.broadcast({{"evt", "clip_saved"},
			{"path", r.path},
			{"game", r.game},
			{"seconds", r.duration},
			{"bytes", r.bytes},
			{"bookmarks", r.bookmarks}});
	cleanup_async();
}

void App::toggle_pause()
{
	paused_ = !paused_;
	update_buffer_state();
	toast(ToastKind::Info, paused_ ? "Buffer paused" : "Buffer resumed",
	      paused_ ? "Nothing is being captured" : "Capturing again",
	      "Press " + settings_.hotkeys.pause.to_string() + " to " + (paused_ ? "resume" : "pause"));
	broadcast_status();
}

void App::toggle_recording()
{
	if (!engine_ok_)
		return;
	std::string err;
	if (!engine_.toggle_recording(&err)) {
		toast(ToastKind::Error, "Recording failed", err);
		if (settings_.overlay.sound)
			play_cue(Cue::Error, settings_.overlay.sound_volume);
	}
}

void App::update_buffer_state()
{
	const auto &c = settings_.clipping;
	bool want = engine_ok_ && !paused_;
	if (want && c.source == "game" && c.auto_start)
		want = c.idle_stop ? (game_running_ || GetTickCount64() < idle_grace_until_) : ever_hooked_;

	if (want && !engine_.buffer_active()) {
		std::string err;
		if (!engine_.start_buffer(&err)) {
			HL_ERROR("Could not start the buffer: %s", err.c_str());
			toast(ToastKind::Error, "Could not start the buffer", err);
		}
	} else if (!want && engine_.buffer_active()) {
		engine_.stop_buffer();
	}
	update_tray();
}

void App::apply_settings(const Settings &s)
{
	const bool autostart_changed = s.clipping.start_with_windows != settings_.clipping.start_with_windows;
	settings_ = s;
	apply_hotkeys();
	if (autostart_changed)
		set_autostart(s.clipping.start_with_windows);

	if (engine_ok_) {
		std::string err;
		if (!engine_.apply(s, &err)) {
			HL_ERROR("Applying settings failed: %s", err.c_str());
			toast(ToastKind::Error, "Settings not applied", err);
			ipc_.broadcast({{"evt", "error"}, {"message", err}});
		}
	} else {
		/* The pipeline failed earlier; the new settings may fix it. */
		std::string err;
		engine_ok_ = engine_.apply(s, &err, true);
		engine_error_ = engine_ok_ ? "" : err;
		if (!engine_ok_)
			toast(ToastKind::Error, "Highlight still cannot capture", err);
	}
	update_buffer_state();
	broadcast_status();
}

/* ------------------------------------------------------------------ */
/* IPC                                                                */

void App::handle_ipc(const ipc::json &msg)
{
	std::string cmd = msg.value("cmd", "");
	const auto &c = settings_.clipping;
	if (cmd == "status") {
		broadcast_status();
	} else if (cmd == "save") {
		std::string which = msg.value("which", "clip");
		int seconds = msg.value("seconds", 0);
		if (seconds <= 0)
			seconds = which == "long" ? c.long_seconds : which == "quick" ? c.quick_seconds : c.clip_seconds;
		save(seconds, "");
	} else if (cmd == "bookmark") {
		on_hotkey(HK_BOOKMARK, true);
	} else if (cmd == "screenshot") {
		on_hotkey(HK_SHOT, true);
	} else if (cmd == "record_toggle") {
		toggle_recording();
	} else if (cmd == "pause_toggle") {
		toggle_pause();
	} else if (cmd == "reload_settings") {
		/* The remembered-games list is owned here (the hotkey adds to it while the
		 * window may hold an older copy): keep ours, and write it back if needed. */
		Settings s = Settings::load();
		const bool differs = s.clipping.always_capture != settings_.clipping.always_capture;
		s.clipping.always_capture = settings_.clipping.always_capture;
		apply_settings(s);
		if (differs)
			settings_.save();
	} else if (cmd == "forget_game") {
		forget_game(msg.value("exe", ""));
	} else if (cmd == "capture_auto") {
		engine_.capture_auto();
		broadcast_status();
	} else if (cmd == "hotkeys_enabled") {
		hotkeys_.set_enabled(msg.value("enabled", true));
	} else if (cmd == "open_ui") {
		launch_ui();
	} else if (cmd == "open_folder") {
		open_clips_folder();
	} else if (cmd == "test_toast") {
		/* Same sequence as a real save: animated "Saving clip", then "Clip saved". */
		ToastContent c;
		c.kind = ToastKind::Working;
		c.title = "Saving clip";
		c.line1 = "Last " + span_text(settings_.clipping.clip_seconds) + " + 2 s after";
		c.line2 = "This is a test banner";
		c.key = settings_.hotkeys.save.to_string();
		if (c.key == "Not set")
			c.key.clear();
		overlay_.show_progress(c, settings_.overlay.position, 2.0);
		if (settings_.overlay.sound)
			play_cue(Cue::Saved, settings_.overlay.sound_volume);
		SetTimer(hwnd_, TIMER_TEST_TOAST, 2600, nullptr);
	} else if (cmd == "quit") {
		PostQuitMessage(0);
	}
}

ipc::json App::status_json()
{
	PROCESS_MEMORY_COUNTERS_EX pmc = {};
	pmc.cb = sizeof(pmc);
	GetProcessMemoryInfo(GetCurrentProcess(), (PROCESS_MEMORY_COUNTERS *)&pmc, sizeof(pmc));

	std::string state = engine_.buffer_active() ? "live" : paused_ ? "paused" : engine_ok_ ? "idle" : "error";
	return {{"evt", "status"},
		{"buffer", state},
		{"game", engine_.game()},
		{"game_running", game_running_},
		{"clip_seconds", settings_.clipping.clip_seconds},
		{"buffered", (double)engine_.buffered_usec() / 1e6},
		{"ram_mb", (double)pmc.WorkingSetSize / (1024.0 * 1024.0)},
		{"private_mb", (double)pmc.PrivateUsage / (1024.0 * 1024.0)},
		{"ring_mb", (double)engine_.ring_bytes() / (1024.0 * 1024.0)},
		{"recording", engine_.recording()},
		{"record_seconds", engine_.recording_seconds()},
		{"encoder", engine_.encoder_label()},
		{"height", engine_.output_height()},
		{"fps", engine_.fps()},
		{"always_capture", settings_.clipping.always_capture},
		{"forced", engine_.forced()},
		{"error", engine_ok_ ? "" : engine_error_}};
}

void App::broadcast_status()
{
	if (ipc_.client_count() > 0)
		ipc_.broadcast(status_json());
}

/* ------------------------------------------------------------------ */
/* tray                                                               */

void App::add_tray()
{
	NOTIFYICONDATAW nid = {sizeof(nid)};
	nid.hWnd = hwnd_;
	nid.uID = 1;
	nid.uFlags = NIF_ICON | NIF_TIP | NIF_MESSAGE | NIF_SHOWTIP;
	nid.uCallbackMessage = WM_APP_TRAY;
	nid.hIcon = icons_[0];
	wcscpy_s(nid.szTip, L"Highlight");
	Shell_NotifyIconW(NIM_ADD, &nid);
	nid.uVersion = NOTIFYICON_VERSION_4;
	Shell_NotifyIconW(NIM_SETVERSION, &nid);
	tray_state_ = -1;
	update_tray();
}

void App::update_tray()
{
	int state;
	if (GetTickCount64() < saved_flash_until_)
		state = 2;
	else if (engine_.recording())
		state = 4;
	else if (engine_.buffer_active())
		state = 1;
	else if (paused_)
		state = 3;
	else
		state = 0;

	std::wstring tip;
	if (engine_.buffer_active()) {
		tip = L"Highlight · buffer live · last " + widen(format_duration(settings_.clipping.clip_seconds));
		std::string game = engine_.game();
		if (!game.empty())
			tip += L" · " + widen(game);
	} else if (paused_) {
		tip = L"Highlight · paused";
	} else if (!engine_ok_) {
		tip = L"Highlight · not capturing";
	} else {
		tip = L"Highlight · waiting for a game";
	}
	if (tip.size() > 120)
		tip.resize(120);

	NOTIFYICONDATAW nid = {sizeof(nid)};
	nid.hWnd = hwnd_;
	nid.uID = 1;
	nid.uFlags = NIF_TIP | NIF_SHOWTIP;
	if (state != tray_state_) {
		nid.uFlags |= NIF_ICON;
		nid.hIcon = icons_[state];
		tray_state_ = state;
	}
	wcsncpy_s(nid.szTip, tip.c_str(), _TRUNCATE);
	Shell_NotifyIconW(NIM_MODIFY, &nid);
}

/* ------------------------------------------------------------------ */
/* capture the game in front                                          */

/* UWP apps sit inside an ApplicationFrameHost window: the real app window is the
 * child owned by another process (what OBS does in ms_get_uwp_actual_window). */
static HWND real_app_window(HWND hwnd)
{
	wchar_t cls[64] = {};
	if (!hwnd || !GetClassNameW(hwnd, cls, 64))
		return hwnd;
	if (wcscmp(cls, L"ApplicationFrameWindow") != 0 && wcscmp(cls, L"WinUIDesktopWin32WindowClass") != 0)
		return hwnd;
	DWORD frame_pid = 0;
	GetWindowThreadProcessId(hwnd, &frame_pid);
	for (HWND child = FindWindowExW(hwnd, nullptr, nullptr, nullptr); child;
	     child = FindWindowExW(hwnd, child, nullptr, nullptr)) {
		DWORD pid = 0;
		GetWindowThreadProcessId(child, &pid);
		if (pid != frame_pid)
			return child;
	}
	return hwnd;
}

/* Can this top-level window be a game? (the desktop, taskbar, shell and our own
 * windows cannot) */
bool App::capture_candidate(HWND hwnd)
{
	if (!hwnd || !IsWindow(hwnd) || !IsWindowVisible(hwnd) || IsIconic(hwnd))
		return false;
	if (GetWindowLongW(hwnd, GWL_STYLE) & WS_CHILD)
		return false;
	if (GetAncestor(hwnd, GA_ROOT) != hwnd)
		return false;
	if (GetWindowLongW(hwnd, GWL_EXSTYLE) & WS_EX_TOOLWINDOW)
		return false;
	BOOL cloaked = FALSE;
	if (SUCCEEDED(DwmGetWindowAttribute(hwnd, DWMWA_CLOAKED, &cloaked, sizeof(cloaked))) && cloaked)
		return false;
	RECT r = {};
	if (!GetWindowRect(hwnd, &r) || r.right - r.left < 160 || r.bottom - r.top < 120)
		return false;
	DWORD pid = 0;
	GetWindowThreadProcessId(hwnd, &pid);
	if (pid == GetCurrentProcessId())
		return false;
	/* judge a UWP app by its real window, not the ApplicationFrameHost frame */
	std::string cls, exe;
	if (!Engine::window_info(real_app_window(hwnd), nullptr, &cls, &exe))
		return false;
	static const char *const shell_classes[] = {"Progman", "WorkerW", "Shell_TrayWnd", "Shell_SecondaryTrayWnd",
						    "NotifyIconOverflowWindow"};
	for (const char *c : shell_classes)
		if (cls == c)
			return false;
	/* our own processes, the shell, and the Windows parts OBS leaves out of its window
	 * lists (libobs/util/windows/window-helpers.c, internal_microsoft_exes) */
	static const char *const not_games[] = {
		"highlight.exe",       "highlight-ui.exe",    "explorer.exe",        "searchhost.exe",
		"taskmgr.exe",         "startmenuexperiencehost.exe", "applicationframehost.exe",
		"peopleexperiencehost.exe", "shellexperiencehost.exe", "microsoft.notes.exe", "systemsettings.exe",
		"textinputhost.exe",   "searchapp.exe",       "video.ui.exe",        "searchui.exe",
		"lockapp.exe",         "cortana.exe",         "gamebar.exe",         "tabtip.exe",
		"time.exe"};
	const std::string e = to_lower(exe);
	for (const char *n : not_games)
		if (e == n)
			return false;
	if (e.rfind("windowsinternal", 0) == 0)
		return false;
	return true;
}

void App::capture_window(HWND hwnd, CaptureReason why)
{
	const bool manual = why == CaptureReason::Hotkey;
	const std::string key = settings_.hotkeys.capture_game.to_string();
	if (!engine_ok_) {
		if (manual)
			toast(ToastKind::Error, "Capture is not running", engine_error_);
		return;
	}
	if (settings_.clipping.source != "game") {
		toast(ToastKind::Info, "Capture mode is not \"Game\"",
		      settings_.clipping.source == "display" ? "Whole screen already records every game"
							     : "One window records the window chosen in settings",
		      "Change it in Clipping & hotkeys → What to capture");
		return;
	}
	if (!capture_candidate(hwnd)) {
		if (manual)
			toast(ToastKind::Error, "No game in front", "Click into the game, then press " + key + " again");
		return;
	}
	hwnd = real_app_window(hwnd); /* capture the app itself, not a UWP frame */
	std::string name, exe, err;
	if (!engine_.capture_window(hwnd, &name, &exe, &err)) {
		if (manual)
			toast(ToastKind::Error, "Could not capture this window", err);
		else
			HL_WARN("Could not capture %s: %s", exe.c_str(), err.c_str());
		return;
	}
	/* Remember games picked by hand (known games are found by name anyway). */
	bool added = false;
	std::string title;
	Engine::window_info(hwnd, &title, nullptr, nullptr);
	const std::string e = to_lower(exe);
	auto &list = settings_.clipping.always_capture;
	if (manual && !e.empty() && !is_known_game(exe, title) && std::find(list.begin(), list.end(), e) == list.end()) {
		list.push_back(e);
		settings_.save();
		added = true;
	}
	/* the buffer starts when the game is hooked (or on the window capture fallback) */
	const char *line1 = why == CaptureReason::Remembered ? "Remembered game: picked up automatically"
			    : why == CaptureReason::KnownGame ? "Picked up automatically (game in a window)"
			    : added ? "Highlight will pick it up by itself next time"
				    : "Highlight picks this game up by itself";
	toast(ToastKind::Info, "Capturing " + name, line1, exe, key);
	broadcast_status();
}

void App::watch_foreground()
{
	if (!engine_ok_ || settings_.clipping.source != "game")
		return;
	HWND fg = GetForegroundWindow();
	if (!capture_candidate(fg)) {
		fg_window_ = nullptr;
		fg_seconds_ = 0;
		return;
	}
	last_candidate_ = fg;
	if (fg != fg_window_) {
		fg_window_ = fg;
		fg_seconds_ = 0;
	}
	fg_seconds_++;

	/* A remembered or known game is in front and automatic detection has not picked
	 * it up (it runs in a window, for example): capture it. Wait a few seconds so a
	 * fullscreen game is found the normal way first. */
	if (paused_ || engine_.game_hooked() || engine_.forced() || fg == last_auto_try_ || fg_seconds_ < 3)
		return;
	std::string exe, title;
	if (!Engine::window_info(real_app_window(fg), &title, nullptr, &exe))
		return;
	const auto &list = settings_.clipping.always_capture;
	const bool remembered = std::find(list.begin(), list.end(), to_lower(exe)) != list.end();
	if (!remembered && !is_known_game(exe, title))
		return;
	/* known games often run fullscreen and get hooked the normal way within a few
	 * seconds: only step in when that has clearly not happened */
	if (!remembered && fg_seconds_ < 8)
		return;
	last_auto_try_ = fg;
	capture_window(fg, remembered ? CaptureReason::Remembered : CaptureReason::KnownGame);
}

void App::forget_game(const std::string &exe)
{
	auto &list = settings_.clipping.always_capture;
	auto it = std::find(list.begin(), list.end(), to_lower(exe));
	if (it == list.end())
		return;
	list.erase(it);
	settings_.save();
	HL_INFO("Forgot remembered game %s", exe.c_str());
	broadcast_status();
}

void App::show_tray_menu()
{
	const auto &h = settings_.hotkeys;
	HMENU menu = CreatePopupMenu();
	std::wstring header = engine_.buffer_active()
				      ? L"Buffer live · last " + widen(format_duration(settings_.clipping.clip_seconds))
				      : (paused_ ? L"Buffer paused" : L"Waiting for a game");
	AppendMenuW(menu, MF_STRING | MF_GRAYED, 0, header.c_str());
	AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
	AppendMenuW(menu, MF_STRING, CMD_SAVE, (L"Save clip\t" + widen(h.save.to_string())).c_str());
	AppendMenuW(menu, MF_STRING, CMD_SAVE_LONG,
		    (L"Save last " + widen(format_duration(settings_.clipping.long_seconds)) + L"\t" +
		     widen(h.save_long.to_string()))
			    .c_str());
	AppendMenuW(menu, MF_STRING, CMD_RECORD,
		    ((engine_.recording() ? L"Stop recording\t" : L"Start full recording\t") + widen(h.record.to_string()))
			    .c_str());
	AppendMenuW(menu, MF_STRING, CMD_PAUSE, paused_ ? L"Resume buffer" : L"Pause buffer");
	if (settings_.clipping.source == "game") {
		std::string title;
		std::wstring label = L"Capture the game in front";
		if (last_candidate_ && IsWindow(last_candidate_) && Engine::window_info(last_candidate_, &title, nullptr, nullptr) &&
		    !title.empty())
			label = L"Capture \"" + widen(title.size() > 40 ? title.substr(0, 40) + "…" : title) + L"\"";
		AppendMenuW(menu, MF_STRING | (last_candidate_ && IsWindow(last_candidate_) ? 0 : MF_GRAYED), CMD_CAPTURE,
			    (label + L"\t" + widen(h.capture_game.to_string())).c_str());
	}
	AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
	AppendMenuW(menu, MF_STRING, CMD_FOLDER, L"Open clips folder");
	AppendMenuW(menu, MF_STRING, CMD_OPEN, L"Open Highlight");
	AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
	AppendMenuW(menu, MF_STRING, CMD_QUIT, L"Quit Highlight");

	POINT pt;
	GetCursorPos(&pt);
	SetForegroundWindow(hwnd_);
	TrackPopupMenu(menu, TPM_RIGHTBUTTON | TPM_BOTTOMALIGN, pt.x, pt.y, 0, hwnd_, nullptr);
	PostMessageW(hwnd_, WM_NULL, 0, 0);
	DestroyMenu(menu);
}

void App::launch_ui()
{
	HANDLE m = OpenMutexW(SYNCHRONIZE, FALSE, L"Local\\HighlightUI");
	if (m) {
		CloseHandle(m);
		ipc_.broadcast({{"evt", "show"}});
		return;
	}
	std::wstring exe = (paths::exe_dir() / L"highlight-ui.exe").wstring();
	std::wstring cmd = L"\"" + exe + L"\"";
	STARTUPINFOW si = {sizeof(si)};
	PROCESS_INFORMATION pi = {};
	if (CreateProcessW(exe.c_str(), cmd.data(), nullptr, nullptr, FALSE, 0, nullptr, paths::exe_dir().c_str(), &si,
			   &pi)) {
		CloseHandle(pi.hThread);
		CloseHandle(pi.hProcess);
	} else {
		HL_ERROR("Could not start the UI (%lu)", GetLastError());
	}
}

void App::open_clips_folder()
{
	std::wstring dir = widen(settings_.clips_dir());
	std::error_code ec;
	fs::create_directories(dir, ec);
	ShellExecuteW(nullptr, L"open", dir.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}

void App::set_autostart(bool on)
{
	HKEY key;
	if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Run", 0, KEY_SET_VALUE,
			  &key) != ERROR_SUCCESS)
		return;
	if (on) {
		std::wstring cmd = L"\"" + (paths::exe_dir() / L"highlight.exe").wstring() + L"\" --background";
		RegSetValueExW(key, L"Highlight", 0, REG_SZ, (const BYTE *)cmd.c_str(),
			       (DWORD)((cmd.size() + 1) * sizeof(wchar_t)));
	} else {
		RegDeleteValueW(key, L"Highlight");
	}
	RegCloseKey(key);
}

void App::cleanup_async()
{
	if (cleaning_.exchange(true))
		return;
	Settings s = settings_;
	std::thread([this, s]() {
		std::error_code ec;
		const fs::path clips = widen(s.clips_dir());
		const fs::path share = widen(s.share_dir());

		/* share copies are throwaway */
		if (s.storage.share_cleanup_days > 0 && fs::is_directory(share, ec)) {
			auto cutoff = fs::file_time_type::clock::now() - std::chrono::hours(24 * s.storage.share_cleanup_days);
			for (auto &e : fs::directory_iterator(share, ec))
				if (e.is_regular_file(ec) && e.last_write_time(ec) < cutoff)
					fs::remove(e.path(), ec);
		}

		if (s.storage.auto_delete && fs::is_directory(clips, ec)) {
			struct Item {
				fs::path path;
				uint64_t size;
				fs::file_time_type time;
			};
			std::vector<Item> items;
			uint64_t total = 0;
			for (auto it = fs::recursive_directory_iterator(clips, fs::directory_options::skip_permission_denied, ec);
			     it != fs::recursive_directory_iterator(); it.increment(ec)) {
				if (ec)
					break;
				if (!it->is_regular_file(ec))
					continue;
				if (it->path().parent_path() == share)
					continue;
				std::wstring ext = it->path().extension().wstring();
				std::transform(ext.begin(), ext.end(), ext.begin(), ::towlower);
				if (ext != L".mp4" && ext != L".mkv" && ext != L".mov" && ext != L".png")
					continue;
				Item item{it->path(), (uint64_t)it->file_size(ec), it->last_write_time(ec)};
				total += item.size;
				items.push_back(item);
			}
			const uint64_t limit = (uint64_t)s.storage.max_gb * 1024ull * 1024ull * 1024ull;
			if (total > limit) {
				Library lib = Library::load();
				std::sort(items.begin(), items.end(), [](const Item &a, const Item &b) { return a.time < b.time; });
				for (const auto &item : items) {
					if (total <= limit)
						break;
					if (s.storage.keep_favorites && lib.is_favorite(narrow(item.path.wstring())))
						continue;
					if (fs::remove(item.path, ec)) {
						total -= item.size;
						HL_INFO("Storage limit: deleted %s", narrow(item.path.wstring()).c_str());
					}
				}
			}
		}
		cleaning_ = false;
	}).detach();
}

void App::toast(ToastKind kind, const std::string &title, const std::string &line1, const std::string &line2,
		const std::string &key)
{
	if (!settings_.overlay.toast && kind != ToastKind::Error)
		return;
	ToastContent c;
	c.kind = kind;
	c.title = title;
	c.line1 = line1;
	c.line2 = line2;
	c.key = key == "Not set" ? "" : key;
	overlay_.show_toast(c, settings_.overlay.position, settings_.overlay.toast_seconds);
}

} // namespace hl

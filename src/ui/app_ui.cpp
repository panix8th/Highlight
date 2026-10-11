#include "app_ui.h"

#include "../common/log.h"
#include "../common/paths.h"
#include "../common/strings.h"

#include <imgui_internal.h>

#include <windows.h>

#include <shellapi.h>
#include <shlobj.h>
#include <tlhelp32.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <map>
#include <vector>

namespace ui {

namespace fs = std::filesystem;
using json = hl::ipc::json;

double now_seconds()
{
	using namespace std::chrono;
	return duration<double>(steady_clock::now().time_since_epoch()).count();
}

/* ------------------------------------------------------------------ */
/* lifecycle                                                          */

bool AppUI::init(HWND hwnd)
{
	hwnd_ = hwnd;
	settings_ = hl::Settings::load();
	library_ = hl::Library::load();
	thumbs_.start();
	scan_library_async();
	try_connect();
	return true;
}

void AppUI::shutdown()
{
	flush_settings(true);
	share_job_.cancel = true;
	trim_job_.cancel = true;
	upload_job_.cancel = true;
	share_job_.join();
	trim_job_.join();
	upload_job_.join();
	links_test_job_.join();
	editor_close();
	if (capturing_)
		send({{"cmd", "hotkeys_enabled"}, {"enabled", true}});
	if (scan_thread_.joinable())
		scan_thread_.join();
	thumbs_.stop();
	core_.close();
}

void AppUI::request_show()
{
	show_requested_ = true;
}

bool AppUI::wants_fast_refresh() const
{
	if (ed_ && ed_->open && ed_->player.playing())
		return true;
	if (share_job_.running || trim_job_.running || upload_job_.running || links_test_job_.running || scanning_)
		return true;
	if (capturing_)
		return true;
	return now_seconds() < notice_until_;
}

bool AppUI::hit_caption(POINT pt) const
{
	RECT rc;
	GetClientRect(hwnd_, &rc);
	const int title_h = (int)px(40);
	const int buttons_w = (int)px(46 * 3);
	if (pt.y < 0 || pt.y >= title_h)
		return false;
	return pt.x < rc.right - buttons_w;
}

/* ------------------------------------------------------------------ */
/* core connection                                                    */

static void launch_core()
{
	std::wstring exe = (hl::paths::exe_dir() / L"highlight.exe").wstring();
	std::wstring cmd = L"\"" + exe + L"\" --background";
	STARTUPINFOW si = {sizeof(si)};
	PROCESS_INFORMATION pi = {};
	if (CreateProcessW(exe.c_str(), cmd.data(), nullptr, nullptr, FALSE, 0, nullptr, hl::paths::exe_dir().c_str(),
			   &si, &pi)) {
		CloseHandle(pi.hThread);
		CloseHandle(pi.hProcess);
	}
}

/* A crashed or frozen capture process can keep its single-instance lock, so a
 * plain start does nothing. End every capture process of this install in this
 * Windows session, then start a fresh one. */
static void restart_core()
{
	const std::wstring core = (hl::paths::exe_dir() / L"highlight.exe").wstring();
	DWORD session = 0;
	ProcessIdToSessionId(GetCurrentProcessId(), &session);
	std::vector<HANDLE> ended;
	HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
	if (snap != INVALID_HANDLE_VALUE) {
		PROCESSENTRY32W pe = {sizeof(pe)};
		for (BOOL more = Process32FirstW(snap, &pe); more; more = Process32NextW(snap, &pe)) {
			if (_wcsicmp(pe.szExeFile, L"highlight.exe") != 0)
				continue;
			DWORD s = 0;
			if (!ProcessIdToSessionId(pe.th32ProcessID, &s) || s != session)
				continue;
			HANDLE h = OpenProcess(PROCESS_TERMINATE | PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE, FALSE,
					       pe.th32ProcessID);
			if (!h)
				continue;
			wchar_t path[MAX_PATH] = {};
			DWORD n = MAX_PATH;
			/* only our capture process: the launcher has the same name in the folder above */
			if (QueryFullProcessImageNameW(h, 0, path, &n) && _wcsicmp(path, core.c_str()) == 0 &&
			    TerminateProcess(h, 1)) {
				ended.push_back(h);
			} else {
				CloseHandle(h);
			}
		}
		CloseHandle(snap);
	}
	for (HANDLE h : ended) {
		WaitForSingleObject(h, 5000);
		CloseHandle(h);
	}
	launch_core();
}

void AppUI::restart_capture()
{
	core_.close();
	status_.connected = false;
	restart_core();
	next_connect_ = now_seconds() + 1.0;
	notice("Restarting capture…", false);
}

void AppUI::try_connect()
{
	if (core_.connected())
		return;
	status_.connected = false;
	double now = now_seconds();
	if (now < next_connect_)
		return;
	if (core_.connect(40)) {
		status_.connected = true;
		send({{"cmd", "status"}});
		return;
	}
	if (!core_launch_tried_) {
		core_launch_tried_ = true;
		launch_core();
	}
	next_connect_ = now + 1.0;
}

void AppUI::send(const json &msg)
{
	if (core_.connected())
		core_.send(msg);
}

void AppUI::poll_core()
{
	json msg;
	while (core_.poll(msg)) {
		std::string evt = msg.value("evt", "");
		if (evt == "status") {
			status_.connected = true;
			status_.buffer = msg.value("buffer", "idle");
			status_.game = msg.value("game", "");
			status_.game_running = msg.value("game_running", false);
			status_.forced = msg.value("forced", false);
			/* the core owns the remembered-games list (its hotkey adds to it) */
			if (auto ac = msg.find("always_capture"); ac != msg.end() && ac->is_array()) {
				std::vector<std::string> list;
				for (const auto &e : *ac)
					if (e.is_string())
						list.push_back(e.get<std::string>());
				settings_.clipping.always_capture = std::move(list);
			}
			status_.buffered = msg.value("buffered", 0.0);
			status_.ram_mb = msg.value("ram_mb", 0.0);
			status_.ring_mb = msg.value("ring_mb", 0.0);
			status_.recording = msg.value("recording", false);
			status_.record_seconds = msg.value("record_seconds", (int64_t)0);
			status_.encoder = msg.value("encoder", "");
			status_.height = msg.value("height", 0);
			status_.fps = msg.value("fps", 0);
			status_.clip_seconds = msg.value("clip_seconds", 60);
			status_.error = msg.value("error", "");
		} else if (evt == "clip_saved" || evt == "screenshot_saved") {
			rescan_ = true;
			std::string path = msg.value("path", "");
			notice((evt == "clip_saved" ? "Saved " : "Screenshot saved ") +
			       hl::narrow(fs::path(hl::widen(path)).filename().wstring()));
		} else if (evt == "clip_failed") {
			notice("Clip not saved: " + msg.value("error", ""), true);
		} else if (evt == "show") {
			request_show();
		} else if (evt == "error") {
			notice(msg.value("message", "Error"), true);
		} else if (evt == "core_exit") {
			core_.close();
			status_.connected = false;
			next_connect_ = now_seconds() + 2.0;
		}
	}
	if (!core_.connected())
		status_.connected = false;
}

/* ------------------------------------------------------------------ */
/* settings                                                           */

void AppUI::mark_settings_dirty()
{
	settings_dirty_ = true;
	settings_dirty_at_ = now_seconds();
}

void AppUI::flush_settings(bool force)
{
	if (!settings_dirty_)
		return;
	if (!force && now_seconds() - settings_dirty_at_ < 0.6)
		return;
	settings_dirty_ = false;
	if (settings_.save())
		send({{"cmd", "reload_settings"}});
	else
		notice("Could not save settings", true);
}

/* ------------------------------------------------------------------ */
/* library scan                                                       */

struct MetaCacheEntry {
	uint64_t stamp = 0;
	double duration = -1;
	int height = 0;
	double fps = 0;
	int bookmarks = 0;
};

static std::map<std::string, MetaCacheEntry> load_meta_cache()
{
	std::map<std::string, MetaCacheEntry> out;
	std::ifstream f(hl::paths::local_data() / L"clip_cache.json");
	if (!f)
		return out;
	try {
		json j = json::parse(f);
		for (auto it = j.begin(); it != j.end(); ++it) {
			MetaCacheEntry e;
			e.stamp = it->value("stamp", (uint64_t)0);
			e.duration = it->value("duration", -1.0);
			e.height = it->value("height", 0);
			e.fps = it->value("fps", 0.0);
			e.bookmarks = it->value("bookmarks", 0);
			out[it.key()] = e;
		}
	} catch (...) {
	}
	return out;
}

static void save_meta_cache(const std::map<std::string, MetaCacheEntry> &cache)
{
	json j = json::object();
	for (const auto &[k, e] : cache)
		j[k] = {{"stamp", e.stamp}, {"duration", e.duration}, {"height", e.height}, {"fps", e.fps}, {"bookmarks", e.bookmarks}};
	std::ofstream f(hl::paths::local_data() / L"clip_cache.json", std::ios::binary | std::ios::trunc);
	f << j.dump();
}

static std::string display_name(const std::string &stem, const std::string &game, int64_t mtime)
{
	/* Default names look like "VALORANT_2026-10-11_21-14-03": show "Clip 21:14" instead. */
	size_t n = stem.size();
	if (n >= 20 && stem[n - 9] == '_' && stem[n - 20] == '_' && stem[n - 3] == '-' && stem[n - 6] == '-') {
		(void)game;
		std::string hhmm = stem.substr(n - 8, 2) + ":" + stem.substr(n - 5, 2);
		return "Clip " + hhmm;
	}
	if (n > 8 && stem.substr(n - 8) == "_session")
		return "Session " + display_name(stem.substr(0, n - 8), game, mtime).substr(5);
	if (n > 5 && stem.substr(n - 5) == "_shot") {
		std::string base = display_name(stem.substr(0, n - 5), game, mtime);
		if (base.rfind("Clip ", 0) == 0)
			return "Screenshot " + base.substr(5);
	}
	(void)mtime;
	return stem;
}

void AppUI::scan_library_async()
{
	if (scanning_.exchange(true))
		return;
	if (scan_thread_.joinable())
		scan_thread_.join();
	const std::string root = settings_.clips_dir();
	const bool per_game = settings_.storage.per_game_folders;
	scan_thread_ = std::thread([this, root, per_game]() {
		std::vector<ClipItem> items;
		std::error_code ec;
		const fs::path rootp = hl::widen(root);
		const fs::path share = rootp / L"_Share";
		uint64_t total = 0;
		if (fs::is_directory(rootp, ec)) {
			for (auto it = fs::recursive_directory_iterator(rootp, fs::directory_options::skip_permission_denied, ec);
			     it != fs::recursive_directory_iterator(); it.increment(ec)) {
				if (ec)
					break;
				if (it->is_directory(ec) && it->path() == share) {
					it.disable_recursion_pending();
					continue;
				}
				if (!it->is_regular_file(ec))
					continue;
				std::wstring ext = it->path().extension().wstring();
				std::transform(ext.begin(), ext.end(), ext.begin(), ::towlower);
				bool video = ext == L".mp4" || ext == L".mkv" || ext == L".mov";
				if (!video && ext != L".png")
					continue;
				ClipItem c;
				c.path = hl::narrow(it->path().wstring());
				c.size = (uint64_t)it->file_size(ec);
				total += c.size;
				auto ft = it->last_write_time(ec);
				auto sys = std::chrono::clock_cast<std::chrono::system_clock>(ft);
				c.mtime = std::chrono::duration_cast<std::chrono::seconds>(sys.time_since_epoch()).count();
				c.stamp = (uint64_t)c.mtime * 1000003ull ^ c.size;
				std::string stem = hl::narrow(it->path().stem().wstring());
				fs::path parent = it->path().parent_path();
				if (parent != rootp)
					c.game = hl::narrow(parent.filename().wstring());
				else {
					size_t us = stem.find('_');
					c.game = us != std::string::npos ? stem.substr(0, us) : "Desktop";
				}
				(void)per_game;
				c.session = stem.find("_session") != std::string::npos;
				c.screenshot = !video;
				c.name = display_name(stem, c.game, c.mtime);
				items.push_back(std::move(c));
			}
		}
		std::sort(items.begin(), items.end(), [](const ClipItem &a, const ClipItem &b) { return a.mtime > b.mtime; });

		auto cache = load_meta_cache();
		for (auto &c : items) {
			auto it = cache.find(c.path);
			if (it != cache.end() && it->second.stamp == c.stamp) {
				c.duration = it->second.duration;
				c.height = it->second.height;
				c.fps = it->second.fps;
				c.bookmarks = it->second.bookmarks;
			}
		}
		{
			std::lock_guard lock(clips_mutex_);
			clips_ = items;
			clips_total_bytes_ = total;
		}

		bool changed = false;
		for (auto &c : items) {
			if (c.screenshot || c.duration >= 0)
				continue;
			hl::media::MediaInfo info = hl::media::probe(c.path);
			MetaCacheEntry e;
			e.stamp = c.stamp;
			e.duration = info.ok ? info.duration : 0;
			e.height = info.height;
			e.fps = info.fps;
			for (auto &m : info.chapters)
				if (m.title.rfind("Bookmark", 0) == 0)
					e.bookmarks++;
			cache[c.path] = e;
			changed = true;
			std::lock_guard lock(clips_mutex_);
			for (auto &x : clips_)
				if (x.path == c.path) {
					x.duration = e.duration;
					x.height = e.height;
					x.fps = e.fps;
					x.bookmarks = e.bookmarks;
				}
		}
		if (changed)
			save_meta_cache(cache);
		scanning_ = false;
	});
}

/* ------------------------------------------------------------------ */
/* helpers                                                            */

void AppUI::show_in_folder(const std::string &path)
{
	std::wstring w = hl::widen(path);
	PIDLIST_ABSOLUTE pidl = ILCreateFromPathW(w.c_str());
	if (pidl) {
		SHOpenFolderAndSelectItems(pidl, 0, nullptr, 0);
		ILFree(pidl);
	} else {
		ShellExecuteW(nullptr, L"open", fs::path(w).parent_path().c_str(), nullptr, nullptr, SW_SHOWNORMAL);
	}
}

void AppUI::delete_clip(const std::string &path)
{
	std::wstring w = hl::widen(path);
	w.push_back(L'\0'); /* SHFileOperation wants a double-null-terminated list */
	SHFILEOPSTRUCTW op = {};
	op.hwnd = hwnd_;
	op.wFunc = FO_DELETE;
	op.pFrom = w.c_str();
	op.fFlags = FOF_ALLOWUNDO | FOF_NOCONFIRMATION | FOF_SILENT | FOF_NOERRORUI;
	if (SHFileOperationW(&op) == 0) {
		library_.forget(path);
		library_.save();
		notice("Moved to the Recycle Bin");
	} else {
		notice("Could not delete the clip", true);
	}
	rescan_ = true;
}

void AppUI::notice(const std::string &text, bool error)
{
	notice_ = text;
	notice_error_ = error;
	notice_until_ = now_seconds() + (error ? 5.0 : 3.0);
}

void AppUI::draw_notice()
{
	if (now_seconds() >= notice_until_ || notice_.empty())
		return;
	ImGuiViewport *vp = ImGui::GetMainViewport();
	ImDrawList *dl = ImGui::GetForegroundDrawList();
	ImVec2 ts = measure(fonts.body_medium, notice_.c_str());
	float w = std::min(ts.x + px(56), vp->Size.x - px(40));
	float h = px(44);
	ImVec2 p(vp->Pos.x + (vp->Size.x - w) / 2, vp->Pos.y + vp->Size.y - h - px(24));
	dl->AddRectFilled(p, ImVec2(p.x + w, p.y + h), col::bg1, px(12));
	dl->AddRect(p, ImVec2(p.x + w, p.y + h), col::line, px(12), 0, px(1));
	status_dot(dl, ImVec2(p.x + px(20), p.y + h / 2), px(4), notice_error_ ? col::muted : col::accent);
	text_ellipsis(dl, fonts.body_medium, ImVec2(p.x + px(36), p.y + (h - fonts.body_medium->FontSize) / 2),
		      w - px(48), col::text, notice_);
}

/* ------------------------------------------------------------------ */
/* frame                                                              */

void AppUI::update_tick()
{
	const double now = now_seconds();
	if (update_check_at_ < 0)
		update_check_at_ = now + 4.0; /* after the window is up */
	if (update_check_at_ > 0 && now >= update_check_at_) {
		update_check_at_ = 0;
		if (settings_.overlay.check_updates)
			updater_.check(false);
	}
	/* results of "Check for updates now" */
	const Updater::State st = updater_.state();
	if (st != update_reported_) {
		update_reported_ = st;
		if (updater_.manual() && st == Updater::State::UpToDate)
			notice("Highlight " HL_VERSION " is the latest version", false);
		else if (st == Updater::State::Failed && (updater_.manual() || !updater_.info().version.empty()))
			notice("Update check failed: " + updater_.error(), true);
		else if (st == Updater::State::Launched)
			close_requested_ = true; /* the installer takes over */
	}
}

void AppUI::draw_update_banner()
{
	const Updater::State st = updater_.state();
	if (st != Updater::State::Available && st != Updater::State::Downloading)
		return;
	const UpdateInfo info = updater_.info();
	if (st == Updater::State::Available && !updater_.manual() && info.version == settings_.overlay.skip_version)
		return;
	const float w = ImGui::GetContentRegionAvail().x;
	begin_card("##update", w, true);
	text(fonts.h2, col::text, "Highlight %s is available", info.version.c_str());
	if (st == Updater::State::Downloading) {
		text_wrapped(fonts.caption, col::muted, w - px(40), "Downloading the installer… %d%%",
			     (int)(updater_.progress() * 100));
		ImVec2 p = ImGui::GetCursorScreenPos();
		ImDrawList *dl = ImGui::GetWindowDrawList();
		const float bw = std::min(w - px(40), px(420));
		dl->AddRectFilled(p, ImVec2(p.x + bw, p.y + px(6)), col::bg0, px(3));
		dl->AddRectFilled(p, ImVec2(p.x + bw * updater_.progress(), p.y + px(6)), col::accent, px(3));
		ImGui::Dummy(ImVec2(bw, px(8)));
	} else {
		const bool installed = Updater::installed_copy() && !info.setup_url.empty();
		text_wrapped(fonts.caption, col::muted, w - px(40),
			     installed ? "You have %s. Update now downloads the installer and restarts Highlight. Your clips "
					 "and settings stay as they are."
				       : "You have %s. Download the new version from GitHub. Your clips and settings stay as "
					 "they are.",
			     HL_VERSION);
		if (primary_button(installed ? "UPDATE NOW##upd" : "DOWNLOAD##upd", ImVec2(px(180), px(44))))
			updater_.install(info);
		ImGui::SameLine();
		if (secondary_button("What's new##updnotes", ImVec2(0, px(44))))
			ShellExecuteW(nullptr, L"open", hl::widen(info.page_url).c_str(), nullptr, nullptr, SW_SHOWNORMAL);
		ImGui::SameLine();
		if (secondary_button("Skip this version##updskip", ImVec2(0, px(44)))) {
			settings_.overlay.skip_version = info.version;
			mark_settings_dirty();
			updater_.dismiss();
		}
	}
	end_card();
	ImGui::Dummy(ImVec2(0, px(4)));
}

void AppUI::frame()
{
	poll_core();
	try_connect();
	update_tick();
	flush_settings(false);
	poll_hotkey_capture();
	finish_jobs();
	if (rescan_.exchange(false))
		scan_library_async();

	if (show_requested_) {
		show_requested_ = false;
		if (IsIconic(hwnd_))
			ShowWindow(hwnd_, SW_RESTORE);
		ShowWindow(hwnd_, SW_SHOW);
		SetForegroundWindow(hwnd_);
	}

	ImGuiViewport *vp = ImGui::GetMainViewport();
	ImGui::SetNextWindowPos(vp->Pos);
	ImGui::SetNextWindowSize(vp->Size);
	ImGui::Begin("##root", nullptr,
		     ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
			     ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoScrollWithMouse);

	draw_titlebar();

	const float title_h = px(40);
	ImGui::SetCursorPos(ImVec2(0, title_h));
	const bool editor = page_ == Page::Editor && ed_ && ed_->open;

	if (!editor) {
		ImGui::PushStyleColor(ImGuiCol_ChildBg, col::bg1);
		ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(px(12), px(12)));
		ImGui::BeginChild("##sidebar", ImVec2(px(232), 0), ImGuiChildFlags_AlwaysUseWindowPadding,
				  ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
		draw_sidebar();
		ImGui::EndChild();
		ImGui::PopStyleVar();
		ImGui::PopStyleColor();
		ImDrawList *dl = ImGui::GetWindowDrawList();
		dl->AddLine(ImVec2(vp->Pos.x + px(232), vp->Pos.y + title_h),
			    ImVec2(vp->Pos.x + px(232), vp->Pos.y + vp->Size.y), col::bg3, px(1));
		ImGui::SameLine(0, 0);
	}

	ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, editor ? ImVec2(px(24), px(16)) : ImVec2(px(32), px(28)));
	ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(px(10), px(14)));
	ImGui::BeginChild("##content", ImVec2(0, 0), ImGuiChildFlags_AlwaysUseWindowPadding);
	draw_update_banner();
	draw_banner();
	switch (page_) {
	case Page::Library:
		library_page();
		break;
	case Page::Editor:
		if (ed_ && ed_->open)
			editor_page();
		else
			page_ = Page::Library;
		break;
	case Page::Clipping:
		clipping_page();
		break;
	case Page::Video:
		video_page();
		break;
	case Page::Audio:
		audio_page();
		break;
	case Page::Overlay:
		overlay_page();
		break;
	case Page::Links:
		links_page();
		break;
	}
	ImGui::Dummy(ImVec2(0, px(16)));
	ImGui::EndChild();
	ImGui::PopStyleVar(2);

	share_dialog();
	ImGui::End();
	draw_upload_bar();
	draw_notice();
}

void AppUI::draw_titlebar()
{
	ImGuiViewport *vp = ImGui::GetMainViewport();
	ImDrawList *dl = ImGui::GetWindowDrawList();
	const float h = px(40);
	ImVec2 o = vp->Pos;
	dl->AddRectFilled(o, ImVec2(o.x + vp->Size.x, o.y + h), col::bg0);
	dl->AddLine(ImVec2(o.x, o.y + h - 1), ImVec2(o.x + vp->Size.x, o.y + h - 1), col::bg3, px(1));

	brand_lockup(dl, ImVec2(o.x + px(14), o.y + (h - px(20)) / 2), px(20), fonts.brand);

	/* status line */
	std::string status;
	ImU32 dot = col::dim;
	if (!status_.connected) {
		status = "CAPTURE NOT RUNNING";
	} else if (status_.recording) {
		status = "RECORDING " + hl::format_duration((double)status_.record_seconds);
		dot = col::accent;
	} else if (status_.buffer == "live") {
		status = "BUFFER LIVE · LAST " + hl::format_duration(settings_.clipping.clip_seconds);
		if (!status_.game.empty())
			status += " · " + status_.game;
		dot = col::accent;
	} else if (status_.buffer == "paused") {
		status = "BUFFER PAUSED";
	} else if (status_.buffer == "error") {
		status = "NOT CAPTURING";
	} else {
		status = "WAITING FOR A GAME";
	}

	ImVec2 ss = measure(fonts.mono_small, status.c_str());
	float cx = o.x + (vp->Size.x - ss.x) / 2;
	status_dot(dl, ImVec2(cx - px(12), o.y + h / 2), px(3.5f), dot);
	dl->AddText(fonts.mono_small, fonts.mono_small->FontSize, ImVec2(cx, o.y + (h - ss.y) / 2), col::muted,
		    status.c_str());

	/* window buttons */
	const float bw = px(46);
	struct Btn {
		const char *id;
		Icon icon;
	} btns[3] = {{"##min", Icon::Minus}, {"##max", Icon::Square}, {"##close", Icon::Close}};
	for (int i = 0; i < 3; i++) {
		ImVec2 bp(o.x + vp->Size.x - bw * (3 - i), o.y);
		ImGui::SetCursorScreenPos(bp);
		bool clicked = ImGui::InvisibleButton(btns[i].id, ImVec2(bw, h));
		bool hov = ImGui::IsItemHovered();
		if (hov)
			dl->AddRectFilled(bp, ImVec2(bp.x + bw, bp.y + h), i == 2 ? IM_COL32(0xC4, 0x2B, 0x1C, 255) : col::bg3);
		draw_icon(dl, btns[i].icon, ImVec2(bp.x + bw / 2, bp.y + h / 2), px(13), hov ? col::text : col::muted);
		if (hov && i == 2 && ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal))
			ImGui::SetTooltip("Close the window (capture keeps running in the tray)");
		if (clicked) {
			if (i == 0)
				ShowWindow(hwnd_, SW_MINIMIZE);
			else if (i == 1)
				ShowWindow(hwnd_, IsZoomed(hwnd_) ? SW_RESTORE : SW_MAXIMIZE);
			else
				close_requested_ = true;
		}
	}
}

void AppUI::draw_sidebar()
{
	size_t count;
	{
		std::lock_guard lock(clips_mutex_);
		count = clips_.size();
	}
	std::string badge = std::to_string(count);

	section_label("LIBRARY");
	if (nav_item("Clips", Icon::Grid, page_ == Page::Library || page_ == Page::Editor, badge.c_str()))
		page_ = Page::Library;
	ImGui::Dummy(ImVec2(0, px(4)));
	section_label("SETTINGS");
	if (nav_item("Clipping & hotkeys", Icon::Scissors, page_ == Page::Clipping))
		page_ = Page::Clipping;
	if (nav_item("Video quality", Icon::Monitor, page_ == Page::Video))
		page_ = Page::Video;
	if (nav_item("Audio", Icon::Speaker, page_ == Page::Audio))
		page_ = Page::Audio;
	if (nav_item("Overlay & alerts", Icon::Bell, page_ == Page::Overlay))
		page_ = Page::Overlay;
	if (nav_item("Share links", Icon::Share, page_ == Page::Links))
		page_ = Page::Links;

	/* buffer card pinned to the bottom */
	const float card_h = px(178);
	const float win_h = ImGui::GetWindowHeight();
	float y = win_h - card_h - px(12);
	if (ImGui::GetCursorPosY() + px(16) < y)
		ImGui::SetCursorPosY(y);

	ImVec2 p = ImGui::GetCursorScreenPos();
	const float w = ImGui::GetContentRegionAvail().x;
	ImDrawList *dl = ImGui::GetWindowDrawList();
	dl->AddRectFilled(p, ImVec2(p.x + w, p.y + card_h), col::bg2, px(12));
	dl->AddRect(p, ImVec2(p.x + w, p.y + card_h), col::line, px(12), 0, px(1));

	const float pad = px(14);
	dl->AddText(fonts.mono_small, fonts.mono_small->FontSize, ImVec2(p.x + pad, p.y + pad), col::dim, "REPLAY BUFFER");
	const char *state = !status_.connected               ? "OFF"
			    : status_.buffer == "live"        ? "LIVE"
			    : status_.buffer == "paused"      ? "PAUSED"
			    : status_.buffer == "error"       ? "ERROR"
							      : "IDLE";
	ImVec2 st = measure(fonts.mono_bold, state);
	ImU32 sc = status_.buffer == "live" ? col::accent_text : col::muted;
	dl->AddText(fonts.mono_bold, fonts.mono_small->FontSize, ImVec2(p.x + w - pad - st.x * 0.85f, p.y + pad), sc, state);
	status_dot(dl, ImVec2(p.x + w - pad - st.x * 0.85f - px(10), p.y + pad + px(6)), px(3.5f),
		   status_.buffer == "live" ? col::accent : col::dim);

	float bx = p.x + pad, by = p.y + pad + px(24);
	float bw2 = (w - pad * 2 - px(8)) / 2;
	auto stat_box = [&](float x, const char *label, const std::string &value) {
		dl->AddRectFilled(ImVec2(x, by), ImVec2(x + bw2, by + px(48)), col::bg0, px(8));
		dl->AddText(fonts.caption, fonts.caption->FontSize, ImVec2(x + px(10), by + px(7)), col::dim, label);
		dl->AddText(fonts.mono_bold, fonts.mono_bold->FontSize, ImVec2(x + px(10), by + px(25)), col::text,
			    value.c_str());
	};
	char ram[32], disk[32];
	snprintf(ram, sizeof(ram), status_.connected ? "%.0f MB" : "—", status_.ram_mb);
	snprintf(disk, sizeof(disk), status_.connected ? "%.2f GB" : "—", status_.ring_mb / 1024.0);
	stat_box(bx, "RAM", ram);
	stat_box(bx + bw2 + px(8), "Disk ring", disk);

	ImGui::SetCursorScreenPos(ImVec2(bx, by + px(60)));
	std::string label = "SAVE LAST " + hl::format_duration(settings_.clipping.clip_seconds) + "##save";
	std::string key = hl::key_name(settings_.hotkeys.save.vk);
	bool live = status_.connected && status_.buffer == "live";
	if (primary_button(label.c_str(), ImVec2(w - pad * 2, px(48)), key.c_str(), live))
		send({{"cmd", "save"}, {"which", "clip"}});
	ImGui::SetCursorScreenPos(ImVec2(p.x, p.y + card_h));
	ImGui::Dummy(ImVec2(w, 0));
}

void AppUI::draw_banner()
{
	if (status_.connected && status_.error.empty())
		return;
	const float w = ImGui::GetContentRegionAvail().x;
	begin_card("##banner", w, true);
	if (!status_.connected) {
		text(fonts.h2, col::text, "Highlight capture is not running");
		text_wrapped(fonts.caption, col::muted, w - px(40),
			     "The background process records your games. It starts with Windows and lives in the tray. "
			     "If it stopped or froze, Restart capture ends it and starts it again.");
		if (secondary_button("Start capture##startcore", ImVec2(0, 0), Icon::Play)) {
			launch_core();
			next_connect_ = now_seconds() + 0.8;
		}
		ImGui::SameLine();
		if (secondary_button("Restart capture##restartcore", ImVec2(0, 0), Icon::Logo))
			restart_capture();
	} else {
		text(fonts.h2, col::text, "Capture could not start");
		text_wrapped(fonts.caption, col::muted, w - px(40), "%s", status_.error.c_str());
		text_wrapped(fonts.caption, col::muted, w - px(40),
			     "Try another encoder or capture mode below. Logs: %%LOCALAPPDATA%%\\Highlight\\logs");
	}
	end_card();
	ImGui::Dummy(ImVec2(0, px(4)));
}

void AppUI::page_header(const char *title, const char *subtitle)
{
	text(fonts.h1, col::text, "%s", title);
	if (subtitle && *subtitle) {
		ImGui::SetCursorPosY(ImGui::GetCursorPosY() - px(8));
		text(fonts.body, col::muted, "%s", subtitle);
	}
	ImGui::Dummy(ImVec2(0, px(2)));
}

} // namespace ui

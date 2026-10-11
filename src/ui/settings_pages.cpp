#include "app_ui.h"

#include "../common/strings.h"

#include <windows.h>
#include <initguid.h>
#include <mmdeviceapi.h>
#include <functiondiscoverykeys_devpkey.h>
#include <psapi.h>
#include <shobjidl.h>

#include <algorithm>
#include <filesystem>
#include <set>

namespace ui {

namespace fs = std::filesystem;

/* ------------------------------------------------------------------ */
/* system lookups                                                     */

static std::vector<std::pair<std::string, std::string>> list_audio_devices(bool capture)
{
	std::vector<std::pair<std::string, std::string>> out;
	out.emplace_back("default", capture ? "Default microphone" : "Default output");
	IMMDeviceEnumerator *en = nullptr;
	if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, __uuidof(IMMDeviceEnumerator),
				    (void **)&en)))
		return out;
	IMMDeviceCollection *coll = nullptr;
	if (SUCCEEDED(en->EnumAudioEndpoints(capture ? eCapture : eRender, DEVICE_STATE_ACTIVE, &coll))) {
		UINT n = 0;
		coll->GetCount(&n);
		for (UINT i = 0; i < n; i++) {
			IMMDevice *dev = nullptr;
			if (FAILED(coll->Item(i, &dev)))
				continue;
			LPWSTR id = nullptr;
			IPropertyStore *props = nullptr;
			if (SUCCEEDED(dev->GetId(&id)) && SUCCEEDED(dev->OpenPropertyStore(STGM_READ, &props))) {
				PROPVARIANT v;
				PropVariantInit(&v);
				if (SUCCEEDED(props->GetValue(PKEY_Device_FriendlyName, &v)) && v.vt == VT_LPWSTR)
					out.emplace_back(hl::narrow(id), hl::narrow(v.pwszVal));
				PropVariantClear(&v);
			}
			if (props)
				props->Release();
			CoTaskMemFree(id);
			dev->Release();
		}
		coll->Release();
	}
	en->Release();
	return out;
}

struct WindowInfo {
	std::string title, cls, exe;
};

static BOOL CALLBACK enum_windows_cb(HWND hwnd, LPARAM lp)
{
	auto *out = (std::vector<WindowInfo> *)lp;
	if (!IsWindowVisible(hwnd) || GetWindow(hwnd, GW_OWNER))
		return TRUE;
	if (GetWindowLongW(hwnd, GWL_EXSTYLE) & WS_EX_TOOLWINDOW)
		return TRUE;
	wchar_t title[256] = {}, cls[256] = {};
	if (GetWindowTextW(hwnd, title, 256) <= 0)
		return TRUE;
	GetClassNameW(hwnd, cls, 256);
	DWORD pid = 0;
	GetWindowThreadProcessId(hwnd, &pid);
	std::string exe;
	if (HANDLE proc = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid)) {
		wchar_t path[MAX_PATH] = {};
		DWORD size = MAX_PATH;
		if (QueryFullProcessImageNameW(proc, 0, path, &size))
			exe = hl::narrow(fs::path(path).filename().wstring());
		CloseHandle(proc);
	}
	if (exe.empty() || hl::to_lower(exe) == "highlight-ui.exe" || hl::to_lower(exe) == "explorer.exe")
		return TRUE;
	out->push_back({hl::narrow(title), hl::narrow(cls), exe});
	return TRUE;
}

static std::vector<WindowInfo> list_windows()
{
	std::vector<WindowInfo> out;
	EnumWindows(enum_windows_cb, (LPARAM)&out);
	return out;
}

static std::string encode_part(const std::string &s)
{
	std::string out;
	for (char c : s)
		out += c == '#' ? "#22" : c == ':' ? "#3A" : std::string(1, c);
	return out;
}

static bool pick_folder(HWND owner, std::string &path)
{
	IFileOpenDialog *dlg = nullptr;
	if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dlg))))
		return false;
	DWORD opts = 0;
	dlg->GetOptions(&opts);
	dlg->SetOptions(opts | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM);
	bool ok = false;
	if (SUCCEEDED(dlg->Show(owner))) {
		IShellItem *item = nullptr;
		if (SUCCEEDED(dlg->GetResult(&item))) {
			PWSTR p = nullptr;
			if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &p))) {
				path = hl::narrow(p);
				ok = true;
				CoTaskMemFree(p);
			}
			item->Release();
		}
	}
	dlg->Release();
	return ok;
}

/* ------------------------------------------------------------------ */
/* hotkey capture                                                     */

void AppUI::hotkey_row(const char *id, const char *label, const char *desc, hl::Hotkey &key, bool accent)
{
	ImGui::PushID(id);
	ImDrawList *dl = ImGui::GetWindowDrawList();
	const float avail = ImGui::GetContentRegionAvail().x;
	ImVec2 p = ImGui::GetCursorScreenPos();
	dl->AddLine(p, ImVec2(p.x + avail, p.y), col::line2, px(1));
	const float row_h = px(58);
	dl->AddText(fonts.body_semibold, fonts.body_semibold->FontSize, ImVec2(p.x, p.y + px(11)), col::text, label);
	if (desc && *desc)
		text_ellipsis(dl, fonts.small, ImVec2(p.x, p.y + px(31)), avail - px(190), col::muted, desc);

	bool listening = capturing_ == &key;
	float bw = px(170);
	ImGui::SetCursorScreenPos(ImVec2(p.x + avail - bw, p.y + (row_h - px(40)) / 2));
	ImGui::BeginGroup();
	/* right-align the button inside the slot */
	ImGui::Dummy(ImVec2(0, 0));
	ImGui::EndGroup();
	ImGui::SetCursorScreenPos(ImVec2(p.x + avail - bw, p.y + (row_h - px(40)) / 2));
	if (hotkey_button("##hk", key, listening, accent)) {
		if (listening) {
			capturing_ = nullptr;
			send({{"cmd", "hotkeys_enabled"}, {"enabled", true}});
		} else {
			capturing_ = &key;
			capture_started_ = now_seconds();
			send({{"cmd", "hotkeys_enabled"}, {"enabled", false}});
		}
	}
	if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal))
		ImGui::SetTooltip("Click, then press the new key. Esc cancels, Backspace clears.");
	ImGui::SetCursorScreenPos(ImVec2(p.x, p.y + row_h));
	ImGui::Dummy(ImVec2(avail, 0));
	ImGui::PopID();
}

void AppUI::poll_hotkey_capture()
{
	if (!capturing_)
		return;
	if (now_seconds() - capture_started_ < 0.25) /* ignore the click that started listening */
		return;
	auto down = [](int vk) { return (GetAsyncKeyState(vk) & 0x8000) != 0; };
	auto finish = [&]() {
		capturing_ = nullptr;
		send({{"cmd", "hotkeys_enabled"}, {"enabled", true}});
		mark_settings_dirty();
	};
	if (down(VK_ESCAPE)) {
		capturing_ = nullptr;
		send({{"cmd", "hotkeys_enabled"}, {"enabled", true}});
		return;
	}
	if (down(VK_BACK)) {
		*capturing_ = hl::Hotkey{};
		finish();
		return;
	}
	for (int vk = 1; vk < 255; vk++) {
		if (vk == VK_LBUTTON || vk == VK_RBUTTON || vk == VK_SHIFT || vk == VK_CONTROL || vk == VK_MENU ||
		    vk == VK_LSHIFT || vk == VK_RSHIFT || vk == VK_LCONTROL || vk == VK_RCONTROL || vk == VK_LMENU ||
		    vk == VK_RMENU || vk == VK_LWIN || vk == VK_RWIN || vk == VK_ESCAPE || vk == VK_BACK)
			continue;
		if (down(vk)) {
			hl::Hotkey k;
			k.vk = (uint32_t)vk;
			k.ctrl = down(VK_CONTROL);
			k.shift = down(VK_SHIFT);
			k.alt = down(VK_MENU);
			k.win = down(VK_LWIN) || down(VK_RWIN);
			*capturing_ = k;
			finish();
			return;
		}
	}
}

/* ------------------------------------------------------------------ */
/* shared layout helper: two columns of cards on wide windows          */

struct Columns {
	float width;
	bool two;
	float col_w;
	Columns()
	{
		width = ImGui::GetContentRegionAvail().x;
		two = width > px(900);
		col_w = two ? (width - px(16)) / 2 : width;
	}
};

/* ------------------------------------------------------------------ */
/* Clipping & hotkeys                                                 */

void AppUI::clipping_page()
{
	auto &c = settings_.clipping;
	auto &h = settings_.hotkeys;
	auto &st = settings_.storage;
	page_header("Clipping & hotkeys", "How far back Highlight reaches, how you save, what it captures and where clips go.");
	Columns cols;

	ImGui::BeginGroup();
	/* clip length */
	begin_card("##len", cols.col_w);
	card_header("Clip length", "How much of the past is saved when you press the key.");
	{
		static const int values[] = {15, 30, 45, 60, 120, 180, 300};
		std::vector<std::string> labels = {"15 s", "30 s", "45 s", "1 min", "2 min", "3 min", "5 min", "Custom"};
		int sel = 7;
		for (int i = 0; i < 7; i++)
			if (values[i] == c.clip_seconds)
				sel = i;
		static bool custom = false;
		if (sel == 7)
			custom = true;
		int shown = custom ? 7 : sel;
		if (pill_grid("len", labels, shown, 4, true)) {
			custom = shown == 7;
			if (!custom)
				c.clip_seconds = values[shown];
			mark_settings_dirty();
		}
		if (custom) {
			if (slider_int("##custom", &c.clip_seconds, 5, 900, "%d s", -1))
				mark_settings_dirty();
		}
	}
	text(fonts.body_semibold, col::text, "Keep recording after the press");
	{
		static const int values[] = {0, 3, 5, 10, 15};
		std::vector<std::string> labels = {"Off", "3 s", "5 s", "10 s", "15 s"};
		int sel = 0;
		for (int i = 0; i < 5; i++)
			if (values[i] == c.post_seconds)
				sel = i;
		if (pill_grid("post", labels, sel, 5, true)) {
			c.post_seconds = values[sel];
			mark_settings_dirty();
		}
	}
	{
		/* visual + estimate */
		ImDrawList *dl = ImGui::GetWindowDrawList();
		const float w = ImGui::GetContentRegionAvail().x;
		ImVec2 p = ImGui::GetCursorScreenPos();
		const float box_h = px(150);
		dl->AddRectFilled(p, ImVec2(p.x + w, p.y + box_h), col::bg0, px(12));
		dl->AddRect(p, ImVec2(p.x + w, p.y + box_h), col::line, px(12), 0, px(1));
		float bx = p.x + px(16), bw = w - px(32), by = p.y + px(22);
		float total = (float)(c.clip_seconds + c.post_seconds);
		float split = bx + bw * c.clip_seconds / std::max(1.0f, total);
		dl->AddRectFilled(ImVec2(bx, by), ImVec2(split, by + px(10)), col::accent, px(5), ImDrawFlags_RoundCornersLeft);
		if (c.post_seconds > 0)
			dl->AddRectFilled(ImVec2(split, by), ImVec2(bx + bw, by + px(10)), col::accent_dim, px(5),
					  ImDrawFlags_RoundCornersRight);
		std::string key = hl::key_name(h.save.vk);
		ImVec2 ks = measure(fonts.mono_bold, key.c_str());
		ImVec2 kp(std::clamp(split - ks.x / 2 - px(8), bx, bx + bw - ks.x - px(16)), by - px(7));
		dl->AddRectFilled(kp, ImVec2(kp.x + ks.x + px(16), kp.y + px(24)), col::text, px(6));
		dl->AddText(fonts.mono_bold, fonts.mono_bold->FontSize, ImVec2(kp.x + px(8), kp.y + px(4)), col::ink, key.c_str());

		std::string line = "Pressing " + key + " saves a " + hl::format_duration(total) + " clip: " +
				   hl::format_duration(c.clip_seconds) + " before, " + hl::format_duration(c.post_seconds) +
				   " after.";
		dl->AddText(fonts.body, fonts.body->FontSize, ImVec2(bx, by + px(28)), col::text, line.c_str(), nullptr, bw);

		double mb = total * settings_.video.bitrate_kbps / 8.0 / 1000.0;
		char disk[64];
		snprintf(disk, sizeof(disk), "≈ %.0f MB", mb * 1.15);
		float sw = (bw - px(8)) / 2, sy = p.y + box_h - px(60);
		auto stat = [&](float x, const char *label, const char *value) {
			dl->AddRectFilled(ImVec2(x, sy), ImVec2(x + sw, sy + px(46)), col::bg2, px(8));
			dl->AddText(fonts.small, fonts.small->FontSize, ImVec2(x + px(10), sy + px(6)), col::dim, label);
			dl->AddText(fonts.mono_bold, fonts.mono_bold->FontSize, ImVec2(x + px(10), sy + px(24)), col::text, value);
		};
		stat(bx, "Disk ring buffer", disk);
		stat(bx + sw + px(8), "RAM", "Flat · any length");
		ImGui::Dummy(ImVec2(w, box_h));
	}
	end_card();

	/* capture source */
	begin_card("##src", cols.col_w);
	card_header("What to capture", "Uses the same capture engine as OBS Studio.");
	if (radio_card("game", "Game", "Hooks the game directly. Lowest overhead, works in fullscreen.", c.source == "game",
		       "BEST")) {
		c.source = "game";
		mark_settings_dirty();
	}
	if (radio_card("display", "Whole screen", "Captures your main display. Works with everything.",
		       c.source == "display")) {
		c.source = "display";
		mark_settings_dirty();
	}
	if (radio_card("window", "One window", "Pick an app window, even when it is behind others.",
		       c.source == "window")) {
		c.source = "window";
		mark_settings_dirty();
	}
	if (c.source == "window") {
		static std::vector<WindowInfo> wins;
		static double listed = 0;
		if (now_seconds() - listed > 3) {
			wins = list_windows();
			listed = now_seconds();
		}
		std::vector<std::string> items;
		int cur = -1;
		for (size_t i = 0; i < wins.size(); i++) {
			items.push_back("[" + wins[i].exe + "] " + wins[i].title);
			std::string spec = encode_part(wins[i].title) + ":" + encode_part(wins[i].cls) + ":" + encode_part(wins[i].exe);
			if (spec == c.window)
				cur = (int)i;
		}
		if (cur < 0 && !c.window.empty()) {
			items.insert(items.begin(), "Saved: " + c.window.substr(c.window.find_last_of(':') + 1));
			cur = 0;
			wins.insert(wins.begin(), WindowInfo{});
		}
		if (combo("Window", &cur, items, -1) && cur >= 0 && !wins[cur].exe.empty()) {
			c.window = encode_part(wins[cur].title) + ":" + encode_part(wins[cur].cls) + ":" + encode_part(wins[cur].exe);
			mark_settings_dirty();
		}
	}
	if (c.source == "game") {
		if (toggle_row("Start buffer when a game launches", "Detects running games and starts capturing on its own.",
			       &c.auto_start))
			mark_settings_dirty();
		if (c.auto_start &&
		    toggle_row("Stop buffer on the desktop", "No capture and no GPU encoding while no game is running.",
			       &c.idle_stop))
			mark_settings_dirty();
	}
	if (toggle_row("Capture mouse cursor", "Off for most games, useful for strategy games and desktop clips.",
		       &c.capture_cursor))
		mark_settings_dirty();
	if (toggle_row("Start with Windows, hidden in tray", "The window only opens when you ask for it.",
		       &c.start_with_windows))
		mark_settings_dirty();
	end_card();
	ImGui::EndGroup();

	if (cols.two)
		ImGui::SameLine(0, px(16));

	ImGui::BeginGroup();
	/* hotkeys */
	begin_card("##keys", cols.col_w);
	card_header("Hotkeys", "Work inside fullscreen games. Each save key can grab its own length.");
	{
		std::string d = "Last " + hl::format_duration(c.clip_seconds) +
				(c.post_seconds ? " + " + std::to_string(c.post_seconds) + " s after the press" : "");
		hotkey_row("save", "Save clip", d.c_str(), h.save, true);
		std::string dl = "Last " + hl::format_duration(c.long_seconds);
		hotkey_row("long", "Save long clip", dl.c_str(), h.save_long);
		std::string dq = "Last " + hl::format_duration(c.quick_seconds);
		hotkey_row("quick", "Quick clip", dq.c_str(), h.save_quick);
		hotkey_row("bm", "Bookmark moment", "Drops a marker on the clip timeline", h.bookmark);
		hotkey_row("shot", "Screenshot", "Full-resolution PNG", h.screenshot);
		hotkey_row("rec", "Start / stop full recording", "Records the whole session to disk", h.record);
		hotkey_row("pause", "Pause / resume buffer", "Stops capturing until pressed again", h.pause);
		for (size_t i = 0; i < h.extra.size(); i++) {
			ImGui::PushID((int)i);
			auto &e = h.extra[i];
			std::string name = e.name.empty() ? "Custom save key" : e.name;
			std::string desc = "Last " + hl::format_duration(e.seconds);
			hotkey_row("extra", name.c_str(), desc.c_str(), e.key);
			ImGui::SetNextItemWidth(px(160));
			if (slider_int("##secs", &e.seconds, 5, 900, "%d s", px(200)))
				mark_settings_dirty();
			ImGui::SameLine();
			if (secondary_button("Remove##x", ImVec2(px(100), px(36)))) {
				h.extra.erase(h.extra.begin() + i);
				mark_settings_dirty();
				ImGui::PopID();
				break;
			}
			ImGui::PopID();
		}
	}
	if (h.extra.size() < 4 &&
	    secondary_button("Add a save key with its own length##add", ImVec2(-1, px(44)), Icon::Plus)) {
		h.extra.push_back(hl::SaveKey{"Save key " + std::to_string(h.extra.size() + 1), hl::Hotkey{}, 120});
		mark_settings_dirty();
	}
	ImGui::Dummy(ImVec2(0, px(2)));
	text(fonts.small_semibold, col::muted, "Long and quick clip lengths");
	if (slider_int("##longs", &c.long_seconds, 30, 1200, "Long: %d s", -1))
		mark_settings_dirty();
	if (slider_int("##quicks", &c.quick_seconds, 5, 120, "Quick: %d s", -1))
		mark_settings_dirty();
	end_card();

	/* storage */
	begin_card("##store", cols.col_w);
	card_header("Save location & storage", "Clips are written straight to your PC. Nothing is uploaded.");
	{
		const float w = ImGui::GetContentRegionAvail().x;
		text(fonts.body_semibold, col::text, "Save clips to");
		value_box(settings_.clips_dir().c_str(), w - px(112));
		ImGui::SameLine(0, px(8));
		if (secondary_button("Change##clips", ImVec2(px(104), px(44)))) {
			std::string p;
			if (pick_folder(hwnd_, p)) {
				st.clips_dir = p;
				mark_settings_dirty();
				rescan_ = true;
			}
		}

		std::vector<std::string> drives = {"Default (" + hl::narrow(fs::path(hl::widen(hl::Settings::defaults().buffer_dir())).root_name().wstring()) + " AppData)"};
		std::vector<std::string> roots = {""};
		DWORD mask = GetLogicalDrives();
		for (int d = 0; d < 26; d++) {
			if (!(mask & (1u << d)))
				continue;
			wchar_t root[4] = {(wchar_t)(L'A' + d), L':', L'\\', 0};
			if (GetDriveTypeW(root) != DRIVE_FIXED)
				continue;
			ULARGE_INTEGER free_bytes;
			std::string label = std::string(1, (char)('A' + d)) + ": drive";
			if (GetDiskFreeSpaceExW(root, &free_bytes, nullptr, nullptr))
				label += " · " + hl::format_bytes(free_bytes.QuadPart) + " free";
			drives.push_back(label);
			roots.push_back(std::string(1, (char)('A' + d)) + ":\\Highlight\\ring");
		}
		int cur = 0;
		for (size_t i = 1; i < roots.size(); i++)
			if (hl::to_lower(roots[i]) == hl::to_lower(st.buffer_dir))
				cur = (int)i;
		if (combo("Rolling buffer drive (an SSD is best)", &cur, drives, -1)) {
			st.buffer_dir = roots[cur];
			mark_settings_dirty();
		}

		text(fonts.body_semibold, col::text, "File name");
		if (input_text("##tmpl", st.name_template, -1, "{game}_{date}_{time}"))
			mark_settings_dirty();
		text(fonts.mono_small, col::muted, "{game} {date} {time}  ->  VALORANT_2026-10-11_21-14-03.mp4");
	}
	if (toggle_row("One folder per game", "Clips\\VALORANT, Clips\\Apex Legends …", &st.per_game_folders))
		mark_settings_dirty();
	if (toggle_row("Delete oldest clips above the limit", "Keeps the clips folder from filling your drive.",
		       &st.auto_delete))
		mark_settings_dirty();
	if (st.auto_delete && slider_int("##maxgb", &st.max_gb, 5, 2000, "Limit: %d GB", -1))
		mark_settings_dirty();
	if (toggle_row("Never auto-delete favourites", "Starred clips are always kept.", &st.keep_favorites))
		mark_settings_dirty();
	{
		ImDrawList *dl = ImGui::GetWindowDrawList();
		const float w = ImGui::GetContentRegionAvail().x;
		uint64_t used = clips_total_bytes_;
		double limit = (double)st.max_gb * 1024 * 1024 * 1024;
		float frac = st.auto_delete ? (float)std::min(1.0, used / limit) : 0.0f;
		std::string lbl = hl::format_bytes(used) + (st.auto_delete ? " of " + std::to_string(st.max_gb) + " GB" : " used");
		text(fonts.mono_small, col::muted, "%s", lbl.c_str());
		ImVec2 p = ImGui::GetCursorScreenPos();
		dl->AddRectFilled(p, ImVec2(p.x + w, p.y + px(8)), col::bg0, px(4));
		dl->AddRectFilled(p, ImVec2(p.x + w * frac, p.y + px(8)), col::accent, px(4));
		ImGui::Dummy(ImVec2(w, px(8)));
	}
	end_card();
	ImGui::EndGroup();
}

/* ------------------------------------------------------------------ */
/* Video                                                              */

void AppUI::video_page()
{
	auto &v = settings_.video;
	page_header("Video quality", "Encoding happens on your graphics card, so quality costs disk space, not frames.");
	const float avail = ImGui::GetContentRegionAvail().x;

	/* presets */
	begin_card("##presets", avail);
	{
		std::string summary = (v.resolution == "native" ? std::string("Native") : v.resolution) + " · " +
				      std::to_string(v.fps) + " fps · " +
				      (status_.encoder.empty() ? std::string("GPU") : status_.encoder) + " · ≈ " +
				      std::to_string((int)(v.bitrate_kbps * 7.5 / 1000)) + " MB per minute";
		card_header("Preset", "Start here. Everything below follows the preset until you change it.");
		text(fonts.mono, col::text, "%s", summary.c_str());
		struct P {
			const char *id, *name, *spec, *rate, *tag;
		} presets[] = {
			{"light", "Lightweight", "1080p · 30 fps", "≈ 75 MB/min", ""},
			{"balanced", "Balanced", "1080p · 60 fps", "≈ 150 MB/min", ""},
			{"high", "High", "1440p · 60 fps", "≈ 300 MB/min", "PICK"},
			{"ultra", "Ultra", "Native · 120 fps", "≈ 600 MB/min", ""},
			{"custom", "Custom", "Your own settings", "—", ""},
		};
		const float w = ImGui::GetContentRegionAvail().x;
		int cols = w > px(760) ? 5 : 3;
		float cw = (w - px(10) * (cols - 1)) / cols;
		ImDrawList *dl = ImGui::GetWindowDrawList();
		for (int i = 0; i < 5; i++) {
			if (i % cols)
				ImGui::SameLine(0, px(10));
			ImVec2 p = ImGui::GetCursorScreenPos();
			ImGui::PushID(i);
			bool clicked = ImGui::InvisibleButton("##p", ImVec2(cw, px(112)));
			bool hov = ImGui::IsItemHovered();
			ImGui::PopID();
			bool sel = v.preset == presets[i].id;
			dl->AddRectFilled(p, ImVec2(p.x + cw, p.y + px(112)), sel ? col::bg3 : col::bg0, px(12));
			dl->AddRect(p, ImVec2(p.x + cw, p.y + px(112)), sel ? col::accent : (hov ? col::key_border : col::line),
				    px(12), 0, px(1));
			dl->AddText(fonts.body_bold, fonts.body_bold->FontSize, ImVec2(p.x + px(14), p.y + px(14)), col::text,
				    presets[i].name);
			if (*presets[i].tag) {
				ImVec2 ts = measure(fonts.mono_small, presets[i].tag);
				dl->AddText(fonts.mono_small, fonts.mono_small->FontSize, ImVec2(p.x + cw - ts.x - px(12), p.y + px(16)),
					    col::accent_text, presets[i].tag);
			}
			dl->AddText(fonts.small, fonts.small->FontSize, ImVec2(p.x + px(14), p.y + px(40)), col::muted,
				    presets[i].spec);
			dl->AddText(fonts.mono_small, fonts.mono_small->FontSize, ImVec2(p.x + px(14), p.y + px(84)), col::text,
				    presets[i].rate);
			if (clicked && !sel) {
				if (strcmp(presets[i].id, "custom") == 0)
					v.preset = "custom";
				else
					hl::apply_video_preset(v, presets[i].id);
				mark_settings_dirty();
			}
		}
	}
	end_card();

	Columns cols;
	ImGui::BeginGroup();
	begin_card("##res", cols.col_w);
	card_header("Resolution & frame rate", "Highlight never upscales: smaller screens record at their own size.");
	{
		const float w = ImGui::GetContentRegionAvail().x;
		static const std::vector<std::string> res_ids = {"native", "1440p", "1080p", "900p", "720p"};
		static const std::vector<std::string> res_names = {"Native", "2560×1440", "1920×1080", "1600×900", "1280×720"};
		int ri = (int)(std::find(res_ids.begin(), res_ids.end(), v.resolution) - res_ids.begin());
		if (ri >= (int)res_ids.size())
			ri = 1;
		if (combo("Output resolution", &ri, res_names, (w - px(10)) / 2)) {
			v.resolution = res_ids[ri];
			v.preset = "custom";
			mark_settings_dirty();
		}
		ImGui::SameLine(0, px(10));
		static const std::vector<std::string> sc_ids = {"lanczos", "bicubic", "bilinear"};
		static const std::vector<std::string> sc_names = {"Lanczos · sharpest", "Bicubic", "Bilinear · fastest"};
		int si = (int)(std::find(sc_ids.begin(), sc_ids.end(), v.scale_filter) - sc_ids.begin());
		if (si >= 3)
			si = 0;
		if (combo("Downscale filter", &si, sc_names, (w - px(10)) / 2)) {
			v.scale_filter = sc_ids[si];
			mark_settings_dirty();
		}
		text(fonts.body_semibold, col::text, "Frame rate");
		static const int fps[] = {30, 60, 90, 120, 144, 165, 240};
		std::vector<std::string> labels = {"30", "60", "90", "120", "144", "165", "240"};
		int fi = 1;
		for (int i = 0; i < 7; i++)
			if (fps[i] == v.fps)
				fi = i;
		if (pill_grid("fps", labels, fi, 7, true)) {
			v.fps = fps[fi];
			v.preset = "custom";
			mark_settings_dirty();
		}
		text(fonts.small, col::muted, "Above 60 fps is for slow-motion edits. File size grows with it.");
	}
	end_card();

	begin_card("##enc", cols.col_w);
	card_header("Encoder", status_.encoder.empty() ? "The encoder runs on your graphics card."
						     : ("In use right now: " + status_.encoder).c_str());
	{
		struct E {
			const char *id, *name, *desc, *tag;
		} encs[] = {
			{"nvenc_hevc", "NVIDIA NVENC · HEVC (H.265)", "Great quality per MB, plays on Windows 10 and 11.", "PICK"},
			{"nvenc_av1", "NVIDIA NVENC · AV1", "Smallest files. Needs an RTX 40-series card or newer.", ""},
			{"nvenc_h264", "NVIDIA NVENC · H.264", "Plays anywhere. Bigger files.", ""},
			{"amf_hevc", "AMD · HEVC", "For Radeon graphics cards.", ""},
			{"qsv_hevc", "Intel Quick Sync · HEVC", "For Intel graphics.", ""},
			{"x264", "x264 · CPU", "Costs game performance. Last resort.", ""},
		};
		for (auto &e : encs)
			if (radio_card(e.id, e.name, e.desc, v.encoder == e.id, e.tag)) {
				v.encoder = e.id;
				mark_settings_dirty();
			}
		text_wrapped(fonts.small, col::muted, ImGui::GetContentRegionAvail().x,
			     "If the picked encoder is not available, Highlight falls back to the best one your PC has.");
	}
	end_card();
	ImGui::EndGroup();

	if (cols.two)
		ImGui::SameLine(0, px(16));

	ImGui::BeginGroup();
	begin_card("##rc", cols.col_w);
	card_header("Rate control", "Constant quality spends bits where the action is.");
	{
		static const std::vector<std::string> rc_ids = {"cq", "vbr", "cbr"};
		int rc = (int)(std::find(rc_ids.begin(), rc_ids.end(), v.rate_control) - rc_ids.begin());
		if (rc >= 3)
			rc = 0;
		if (segmented("rc", {"Constant quality", "VBR", "CBR"}, rc)) {
			v.rate_control = rc_ids[rc];
			v.preset = "custom";
			mark_settings_dirty();
		}
		if (v.rate_control == "cq") {
			text(fonts.body_semibold, col::text, "Quality level");
			if (slider_int("##cq", &v.quality, 14, 32, "CQ %d", -1)) {
				v.preset = "custom";
				mark_settings_dirty();
			}
			text(fonts.small, col::muted, "Lower is sharper. 18-22 looks like the game itself.");
		}
		text(fonts.body_semibold, col::text, v.rate_control == "cq" ? "Bitrate cap" : "Bitrate");
		if (slider_int("##br", &v.bitrate_kbps, 4000, 150000, "%d kbps", -1)) {
			v.preset = "custom";
			mark_settings_dirty();
		}
		const float w = ImGui::GetContentRegionAvail().x;
		std::vector<std::string> keys = {"1 s · tight trims", "2 s", "3 s"};
		int ki = std::clamp(v.keyint_sec, 1, 3) - 1;
		if (combo("Keyframe every", &ki, keys, (w - px(10)) / 2)) {
			v.keyint_sec = ki + 1;
			mark_settings_dirty();
		}
		ImGui::SameLine(0, px(10));
		std::vector<std::string> presets = {"P1 · fastest", "P2", "P3", "P4 · balanced", "P5 · quality", "P6", "P7 · max quality"};
		int pi = std::clamp(v.nvenc_preset.size() == 2 ? v.nvenc_preset[1] - '1' : 4, 0, 6);
		if (combo("NVENC preset", &pi, presets, (w - px(10)) / 2)) {
			v.nvenc_preset = "p" + std::to_string(pi + 1);
			mark_settings_dirty();
		}
	}
	if (toggle_row("Look-ahead", "Better quality in fast motion. Uses a little more GPU memory.", &v.lookahead))
		mark_settings_dirty();
	if (toggle_row("Psycho-visual tuning", "Keeps detail where your eye looks.", &v.psycho_visual))
		mark_settings_dirty();
	end_card();

	begin_card("##file", cols.col_w);
	card_header("File & colour", "How the clip is packaged on disk.");
	{
		static const std::vector<std::string> ids = {"mp4", "mkv", "mov"};
		int ci = (int)(std::find(ids.begin(), ids.end(), v.container) - ids.begin());
		if (ci >= 3)
			ci = 0;
		if (pill_grid("cont", {"MP4", "MKV", "MOV"}, ci, 3, true)) {
			v.container = ids[ci];
			mark_settings_dirty();
		}
		const char *note = ci == 0   ? "Plays everywhere. Clips are written in one go, so a crash never leaves a broken file."
				   : ci == 1 ? "Safest for long recordings."
					     : "For editing apps that prefer QuickTime files.";
		text_wrapped(fonts.small, col::muted, ImGui::GetContentRegionAvail().x, "%s", note);
	}
	if (toggle_row("Capture HDR", "10-bit HEVC or AV1 for HDR games and monitors.", &v.hdr))
		mark_settings_dirty();
	end_card();
	ImGui::EndGroup();
}

/* ------------------------------------------------------------------ */
/* Audio                                                              */

void AppUI::audio_page()
{
	auto &a = settings_.audio;
	page_header("Audio", "Each source lands on its own track, so you can fix the mix after the moment.");
	if (!devices_loaded_) {
		out_devices_ = list_audio_devices(false);
		in_devices_ = list_audio_devices(true);
		devices_loaded_ = true;
	}
	const float avail = ImGui::GetContentRegionAvail().x;

	begin_card("##tracks", avail);
	card_header("Sources & tracks", "Capture a whole device, or just one app's sound. Up to five tracks.");
	{
		const float w = ImGui::GetContentRegionAvail().x;
		ImDrawList *dl = ImGui::GetWindowDrawList();
		int remove = -1;
		for (size_t i = 0; i < a.tracks.size(); i++) {
			auto &t = a.tracks[i];
			ImGui::PushID((int)i);
			ImVec2 p = ImGui::GetCursorScreenPos();
			dl->AddLine(p, ImVec2(p.x + w, p.y), col::line2, px(1));
			ImGui::Dummy(ImVec2(0, px(6)));

			std::string tag = "T" + std::to_string(i + (a.layout == "both" ? 2 : 1));
			ImVec2 tp = ImGui::GetCursorScreenPos();
			ImVec2 ts = measure(fonts.mono_small, tag.c_str());
			dl->AddRectFilled(ImVec2(tp.x, tp.y + px(14)), ImVec2(tp.x + ts.x + px(12), tp.y + px(32)),
					  t.enabled ? col::text : col::ring, px(5));
			dl->AddText(fonts.mono_bold, fonts.mono_small->FontSize, ImVec2(tp.x + px(6), tp.y + px(16)), col::ink,
				    tag.c_str());
			ImGui::SetCursorScreenPos(ImVec2(tp.x + px(44), tp.y));
			float name_w = std::min(px(200), w * 0.25f);
			if (input_text("##name", t.name, name_w))
				mark_settings_dirty();
			ImGui::SameLine(0, px(10));

			/* source */
			std::vector<std::string> kinds = {"Game (only the game's sound)", "Device output (everything you hear)",
							  "Microphone", "One app"};
			static const char *kind_ids[] = {"game", "output", "input", "app"};
			int ki = 0;
			for (int k = 0; k < 4; k++)
				if (t.kind == kind_ids[k])
					ki = k;
			float src_w = std::min(px(260), w * 0.3f);
			if (combo("##kind", &ki, kinds, src_w)) {
				t.kind = kind_ids[ki];
				mark_settings_dirty();
			}
			ImGui::SameLine(0, px(10));
			float detail_w = w - (px(44) + name_w + src_w + px(10) * 4 + px(52) + px(44) + px(150));
			detail_w = std::max(detail_w, px(120));
			if (t.kind == "output" || t.kind == "input") {
				auto &devs = t.kind == "input" ? in_devices_ : out_devices_;
				std::vector<std::string> names;
				int cur = 0;
				for (size_t d = 0; d < devs.size(); d++) {
					names.push_back(devs[d].second);
					if (devs[d].first == t.device)
						cur = (int)d;
				}
				if (combo("##dev", &cur, names, detail_w)) {
					t.device = devs[cur].first;
					mark_settings_dirty();
				}
			} else if (t.kind == "app") {
				if (input_text("##exe", t.app_exe, detail_w, "Discord.exe"))
					mark_settings_dirty();
			} else {
				value_box(settings_.clipping.source == "game" ? "Follows the detected game" : "Default output",
					  detail_w, false, col::muted);
			}
			ImGui::SameLine(0, px(10));
			if (slider_int("##vol", &t.volume, 0, 150, "%d%%", px(150)))
				mark_settings_dirty();
			ImGui::SameLine(0, px(4));
			if (toggle("##on", &t.enabled))
				mark_settings_dirty();
			ImGui::SameLine(0, px(0));
			if (icon_button("##rm", Icon::Close, px(44), "Remove this track"))
				remove = (int)i;
			ImGui::PopID();
		}
		if (remove >= 0) {
			a.tracks.erase(a.tracks.begin() + remove);
			mark_settings_dirty();
		}
		if (a.tracks.size() < 5) {
			if (secondary_button("Add app audio##addapp", ImVec2(0, px(44)), Icon::Plus)) {
				a.tracks.push_back(hl::AudioTrack{"app", "App", "", "", true, 100});
				mark_settings_dirty();
			}
			ImGui::SameLine();
			if (secondary_button("Add microphone##addmic", ImVec2(0, px(44)), Icon::Mic)) {
				a.tracks.push_back(hl::AudioTrack{"input", "Microphone 2", "default", "", true, 100});
				mark_settings_dirty();
			}
		}
	}
	end_card();

	Columns cols;
	ImGui::BeginGroup();
	begin_card("##layout", cols.col_w);
	card_header("How tracks are saved", "Some players only play the first audio track.");
	struct L {
		const char *id, *title, *desc, *tag;
	} layouts[] = {
		{"both", "Mixed + separate", "Track 1 has everything mixed, so it plays anywhere. The other tracks keep each source apart for editing.", "PICK"},
		{"mixed", "One mixed track", "Smallest files, best compatibility. No fixing the mix later.", ""},
		{"separate", "Separate tracks only", "For editing apps. Some players will only play the first source.", ""},
	};
	for (auto &l : layouts)
		if (radio_card(l.id, l.title, l.desc, a.layout == l.id, l.tag)) {
			a.layout = l.id;
			mark_settings_dirty();
		}
	end_card();

	begin_card("##quality", cols.col_w);
	{
		std::string sub = (a.codec == "aac" ? std::string("AAC") : std::string("Opus")) + " · " + std::to_string(a.bitrate) +
				  " kbps · " + (a.sample_rate == 48000 ? "48" : "44.1") + " kHz";
		card_header("Audio quality", sub.c_str());
	}
	{
		int ci = a.codec == "opus" ? 1 : 0;
		text(fonts.body_semibold, col::text, "Codec");
		if (pill_grid("codec", {"AAC", "Opus"}, ci, 2, true)) {
			a.codec = ci ? "opus" : "aac";
			mark_settings_dirty();
		}
		static const int rates[] = {128, 160, 192, 256, 320};
		int bi = 2;
		for (int i = 0; i < 5; i++)
			if (rates[i] == a.bitrate)
				bi = i;
		text(fonts.body_semibold, col::text, "Bitrate per track");
		if (pill_grid("abr", {"128", "160", "192", "256", "320"}, bi, 5, true)) {
			a.bitrate = rates[bi];
			mark_settings_dirty();
		}
		int si = a.sample_rate == 44100 ? 0 : 1;
		text(fonts.body_semibold, col::text, "Sample rate");
		if (pill_grid("sr", {"44.1 kHz", "48 kHz"}, si, 2, true)) {
			a.sample_rate = si ? 48000 : 44100;
			mark_settings_dirty();
		}
	}
	end_card();
	ImGui::EndGroup();

	if (cols.two)
		ImGui::SameLine(0, px(16));

	ImGui::BeginGroup();
	begin_card("##mic", cols.col_w);
	card_header("Microphone", "Cleaned up before it reaches the clip.");
	if (toggle_row("Noise suppression", "Removes keyboard clicks, fans and background hum.", &a.noise_suppression))
		mark_settings_dirty();
	if (toggle_row("Noise gate", "Silences the mic when you are not talking.", &a.noise_gate))
		mark_settings_dirty();
	if (toggle_row("Push-to-talk in clips", "Your mic is only recorded while the key below is held.", &a.push_to_talk))
		mark_settings_dirty();
	if (a.push_to_talk)
		hotkey_row("ptt", "Push-to-talk key", "Hold to record your mic", settings_.hotkeys.push_to_talk);
	if (toggle_row("Mute mic in saved clips", "Your voice is left out of clips entirely.", &a.mute_mic_in_clips))
		mark_settings_dirty();
	end_card();
	ImGui::EndGroup();
}

/* ------------------------------------------------------------------ */
/* Overlay & alerts                                                   */

void AppUI::overlay_page()
{
	auto &o = settings_.overlay;
	page_header("Overlay & alerts", "What you see and hear in game when Highlight does something.");
	Columns cols;

	ImGui::BeginGroup();
	begin_card("##toast", cols.col_w);
	card_header("In-game pop-up", "A small card confirms every clip. It never takes focus or blocks clicks.");
	if (toggle_row("Show pop-ups", "Works in borderless and windowed games.", &o.toast))
		mark_settings_dirty();
	{
		static const std::vector<std::string> ids = {"top-left", "top-right", "bottom-left", "bottom-right"};
		int pi = (int)(std::find(ids.begin(), ids.end(), o.position) - ids.begin());
		if (pi >= 4)
			pi = 1;
		text(fonts.body_semibold, col::text, "Position");
		if (pill_grid("pos", {"Top left", "Top right", "Bottom left", "Bottom right"}, pi, 2)) {
			o.position = ids[pi];
			mark_settings_dirty();
		}
		text(fonts.body_semibold, col::text, "Show for");
		if (slider_int("##secs", &o.toast_seconds, 1, 10, "%d s", -1))
			mark_settings_dirty();
	}
	if (secondary_button("Show a test pop-up##test", ImVec2(-1, px(44)), Icon::Bell)) {
		flush_settings(true);
		send({{"cmd", "test_toast"}});
	}
	end_card();
	ImGui::EndGroup();

	if (cols.two)
		ImGui::SameLine(0, px(16));

	ImGui::BeginGroup();
	begin_card("##sound", cols.col_w);
	card_header("Sounds", "A short chime when the key is pressed and when a clip fails.");
	if (toggle_row("Play sounds", "So you know the key worked, even in exclusive fullscreen.", &o.sound))
		mark_settings_dirty();
	if (o.sound && slider_int("##vol", &o.sound_volume, 0, 100, "Volume %d%%", -1))
		mark_settings_dirty();
	if (toggle_row("REC indicator while recording", "A small dot and timer in the top-left corner.", &o.rec_indicator))
		mark_settings_dirty();
	end_card();

	begin_card("##about", cols.col_w, true);
	text(fonts.h2, col::text, "Highlight %s", HL_VERSION);
	text_wrapped(fonts.small, col::muted, ImGui::GetContentRegionAvail().x,
		     "Open source under the GPL. Built on the OBS Studio capture engine and FFmpeg. "
		     "Clips never leave your PC unless you share them.");
	if (secondary_button("Quit Highlight completely##quit", ImVec2(-1, px(44)), Icon::Close)) {
		send({{"cmd", "quit"}});
		close_requested_ = true;
	}
	end_card();
	ImGui::EndGroup();
}

} // namespace ui

#include "settings.h"
#include "log.h"
#include "paths.h"
#include "strings.h"

#include <windows.h>
#include <wincrypt.h>

#include <algorithm>
#include <fstream>
#include <nlohmann/json.hpp>

using json = nlohmann::json;

namespace hl {

/* ------------------------------------------------------------------ */
/* secrets: encrypted with DPAPI, readable only by this Windows user  */

static std::string protect_secret(const std::string &plain)
{
	if (plain.empty())
		return {};
	DATA_BLOB in = {(DWORD)plain.size(), (BYTE *)plain.data()}, out = {};
	if (!CryptProtectData(&in, L"Highlight", nullptr, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &out))
		return {};
	DWORD len = 0;
	std::string b64;
	if (CryptBinaryToStringA(out.pbData, out.cbData, CRYPT_STRING_BASE64 | CRYPT_STRING_NOCRLF, nullptr, &len)) {
		b64.resize(len);
		if (CryptBinaryToStringA(out.pbData, out.cbData, CRYPT_STRING_BASE64 | CRYPT_STRING_NOCRLF, b64.data(), &len))
			b64.resize(len);
		else
			b64.clear();
	}
	LocalFree(out.pbData);
	return b64;
}

static std::string unprotect_secret(const std::string &b64)
{
	if (b64.empty())
		return {};
	DWORD len = 0;
	if (!CryptStringToBinaryA(b64.c_str(), (DWORD)b64.size(), CRYPT_STRING_BASE64, nullptr, &len, nullptr, nullptr))
		return {};
	std::string raw(len, '\0');
	if (!CryptStringToBinaryA(b64.c_str(), (DWORD)b64.size(), CRYPT_STRING_BASE64, (BYTE *)raw.data(), &len, nullptr,
				  nullptr))
		return {};
	DATA_BLOB in = {len, (BYTE *)raw.data()}, out = {};
	if (!CryptUnprotectData(&in, nullptr, nullptr, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &out))
		return {}; /* another user or PC: the secret has to be entered again */
	std::string plain((const char *)out.pbData, out.cbData);
	SecureZeroMemory(out.pbData, out.cbData);
	LocalFree(out.pbData);
	return plain;
}

/* ------------------------------------------------------------------ */
/* key names                                                          */

struct KeyName {
	uint32_t vk;
	const char *name;
};

static const KeyName k_key_names[] = {
	{VK_LBUTTON, "Mouse1"},     {VK_RBUTTON, "Mouse2"},     {VK_MBUTTON, "Mouse3"},
	{VK_XBUTTON1, "Mouse4"},    {VK_XBUTTON2, "Mouse5"},    {VK_BACK, "Backspace"},
	{VK_TAB, "Tab"},            {VK_RETURN, "Enter"},       {VK_PAUSE, "Pause"},
	{VK_CAPITAL, "CapsLock"},   {VK_ESCAPE, "Esc"},         {VK_SPACE, "Space"},
	{VK_PRIOR, "PageUp"},       {VK_NEXT, "PageDown"},      {VK_END, "End"},
	{VK_HOME, "Home"},          {VK_LEFT, "Left"},          {VK_UP, "Up"},
	{VK_RIGHT, "Right"},        {VK_DOWN, "Down"},          {VK_SNAPSHOT, "PrintScreen"},
	{VK_INSERT, "Insert"},      {VK_DELETE, "Delete"},      {VK_NUMPAD0, "Num0"},
	{VK_NUMPAD1, "Num1"},       {VK_NUMPAD2, "Num2"},       {VK_NUMPAD3, "Num3"},
	{VK_NUMPAD4, "Num4"},       {VK_NUMPAD5, "Num5"},       {VK_NUMPAD6, "Num6"},
	{VK_NUMPAD7, "Num7"},       {VK_NUMPAD8, "Num8"},       {VK_NUMPAD9, "Num9"},
	{VK_MULTIPLY, "Num*"},      {VK_ADD, "Num+"},           {VK_SUBTRACT, "Num-"},
	{VK_DECIMAL, "Num."},       {VK_DIVIDE, "Num/"},        {VK_SCROLL, "ScrollLock"},
	{VK_OEM_1, ";"},            {VK_OEM_PLUS, "="},         {VK_OEM_COMMA, ","},
	{VK_OEM_MINUS, "-"},        {VK_OEM_PERIOD, "."},       {VK_OEM_2, "/"},
	{VK_OEM_3, "`"},            {VK_OEM_4, "["},            {VK_OEM_5, "\\"},
	{VK_OEM_6, "]"},            {VK_OEM_7, "'"},            {VK_MEDIA_PLAY_PAUSE, "MediaPlay"},
	{VK_MEDIA_NEXT_TRACK, "MediaNext"},                     {VK_MEDIA_PREV_TRACK, "MediaPrev"},
};

std::string key_name(uint32_t vk)
{
	if (vk == 0)
		return "";
	if (vk >= VK_F1 && vk <= VK_F24)
		return "F" + std::to_string(vk - VK_F1 + 1);
	if ((vk >= '0' && vk <= '9') || (vk >= 'A' && vk <= 'Z'))
		return std::string(1, (char)vk);
	for (const auto &k : k_key_names)
		if (k.vk == vk)
			return k.name;
	return "Key" + std::to_string(vk);
}

uint32_t key_from_name(const std::string &name)
{
	if (name.empty())
		return 0;
	if (name.size() >= 2 && (name[0] == 'F' || name[0] == 'f') && isdigit((unsigned char)name[1])) {
		int n = atoi(name.c_str() + 1);
		if (n >= 1 && n <= 24)
			return VK_F1 + n - 1;
	}
	if (name.size() == 1) {
		char c = (char)toupper((unsigned char)name[0]);
		if ((c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z'))
			return (uint32_t)c;
	}
	for (const auto &k : k_key_names)
		if (_stricmp(k.name, name.c_str()) == 0)
			return k.vk;
	if (name.rfind("Key", 0) == 0)
		return (uint32_t)atoi(name.c_str() + 3);
	return 0;
}

std::string Hotkey::to_string() const
{
	if (vk == 0)
		return "Not set";
	std::string s;
	if (ctrl)
		s += "Ctrl + ";
	if (shift)
		s += "Shift + ";
	if (alt)
		s += "Alt + ";
	if (win)
		s += "Win + ";
	return s + key_name(vk);
}

/* ------------------------------------------------------------------ */
/* JSON mapping                                                       */

static void to_json(json &j, const Hotkey &h)
{
	j = json{{"key", key_name(h.vk)}, {"ctrl", h.ctrl}, {"shift", h.shift}, {"alt", h.alt}, {"win", h.win}};
}

static void from_json(const json &j, Hotkey &h)
{
	h = Hotkey{};
	if (!j.is_object())
		return;
	h.vk = key_from_name(j.value("key", ""));
	h.ctrl = j.value("ctrl", false);
	h.shift = j.value("shift", false);
	h.alt = j.value("alt", false);
	h.win = j.value("win", false);
}

template<typename T> static void read(const json &j, const char *key, T &out)
{
	auto it = j.find(key);
	if (it == j.end())
		return;
	try {
		out = it->get<T>();
	} catch (...) {
	}
}

static void read_hotkey(const json &j, const char *key, Hotkey &out)
{
	auto it = j.find(key);
	if (it != j.end())
		from_json(*it, out);
}

static json to_json(const Settings &s)
{
	json j;

	const auto &c = s.clipping;
	j["clipping"] = {{"clip_seconds", c.clip_seconds},     {"post_seconds", c.post_seconds},
			 {"long_seconds", c.long_seconds},     {"quick_seconds", c.quick_seconds},
			 {"source", c.source},                 {"display_id", c.display_id},
			 {"window", c.window},                 {"auto_start", c.auto_start},
			 {"idle_stop", c.idle_stop},           {"capture_cursor", c.capture_cursor},
			 {"start_with_windows", c.start_with_windows}, {"always_capture", c.always_capture}};

	const auto &h = s.hotkeys;
	json hk;
	to_json(hk["save"], h.save);
	to_json(hk["save_long"], h.save_long);
	to_json(hk["save_quick"], h.save_quick);
	to_json(hk["bookmark"], h.bookmark);
	to_json(hk["screenshot"], h.screenshot);
	to_json(hk["record"], h.record);
	to_json(hk["pause"], h.pause);
	to_json(hk["push_to_talk"], h.push_to_talk);
	to_json(hk["capture_game"], h.capture_game);
	json extra = json::array();
	for (const auto &e : h.extra) {
		json k;
		to_json(k, e.key);
		extra.push_back({{"name", e.name}, {"key", k}, {"seconds", e.seconds}});
	}
	hk["extra"] = extra;
	j["hotkeys"] = hk;

	const auto &st = s.storage;
	j["storage"] = {{"clips_dir", st.clips_dir},
			{"buffer_dir", st.buffer_dir},
			{"name_template", st.name_template},
			{"per_game_folders", st.per_game_folders},
			{"auto_delete", st.auto_delete},
			{"max_gb", st.max_gb},
			{"keep_favorites", st.keep_favorites},
			{"share_cleanup_days", st.share_cleanup_days}};

	const auto &v = s.video;
	j["video"] = {{"preset", v.preset},
		      {"resolution", v.resolution},
		      {"fps", v.fps},
		      {"encoder", v.encoder},
		      {"rate_control", v.rate_control},
		      {"quality", v.quality},
		      {"bitrate_kbps", v.bitrate_kbps},
		      {"keyint_sec", v.keyint_sec},
		      {"nvenc_preset", v.nvenc_preset},
		      {"lookahead", v.lookahead},
		      {"psycho_visual", v.psycho_visual},
		      {"container", v.container},
		      {"hdr", v.hdr},
		      {"scale_filter", v.scale_filter}};

	const auto &a = s.audio;
	json tracks = json::array();
	for (const auto &t : a.tracks)
		tracks.push_back({{"kind", t.kind},
				  {"name", t.name},
				  {"device", t.device},
				  {"app_exe", t.app_exe},
				  {"enabled", t.enabled},
				  {"volume", t.volume}});
	j["audio"] = {{"tracks", tracks},
		      {"layout", a.layout},
		      {"codec", a.codec},
		      {"bitrate", a.bitrate},
		      {"sample_rate", a.sample_rate},
		      {"noise_suppression", a.noise_suppression},
		      {"noise_gate", a.noise_gate},
		      {"push_to_talk", a.push_to_talk},
		      {"mute_mic_in_clips", a.mute_mic_in_clips}};

	const auto &o = s.overlay;
	j["overlay"] = {{"toast", o.toast},
			{"position", o.position},
			{"toast_seconds", o.toast_seconds},
			{"sound", o.sound},
			{"sound_volume", o.sound_volume},
			{"rec_indicator", o.rec_indicator},
			{"check_updates", o.check_updates},
			{"skip_version", o.skip_version}};

	const auto &sh = s.share;
	j["share"] = {{"target", sh.target},
		      {"codec", sh.codec},
		      {"discord_mb", sh.discord_mb},
		      {"nitro_basic_mb", sh.nitro_basic_mb},
		      {"nitro_mb", sh.nitro_mb},
		      {"custom_mb", sh.custom_mb},
		      {"copy_to_clipboard", sh.copy_to_clipboard},
		      {"upload_link", sh.upload_link},
		      {"r2_account", sh.r2_account},
		      {"r2_access_key", sh.r2_access_key},
		      {"r2_secret_dpapi", protect_secret(sh.r2_secret)},
		      {"r2_bucket", sh.r2_bucket},
		      {"r2_public_url", sh.r2_public_url}};

	j["version"] = 1;
	return j;
}

static void from_json(const json &j, Settings &s)
{
	if (auto it = j.find("clipping"); it != j.end() && it->is_object()) {
		auto &c = s.clipping;
		read(*it, "clip_seconds", c.clip_seconds);
		read(*it, "post_seconds", c.post_seconds);
		read(*it, "long_seconds", c.long_seconds);
		read(*it, "quick_seconds", c.quick_seconds);
		read(*it, "source", c.source);
		read(*it, "display_id", c.display_id);
		read(*it, "window", c.window);
		read(*it, "auto_start", c.auto_start);
		read(*it, "idle_stop", c.idle_stop);
		read(*it, "capture_cursor", c.capture_cursor);
		read(*it, "start_with_windows", c.start_with_windows);
		if (auto ac = it->find("always_capture"); ac != it->end() && ac->is_array()) {
			c.always_capture.clear();
			for (const auto &e : *ac)
				if (e.is_string() && !e.get<std::string>().empty())
					c.always_capture.push_back(to_lower(e.get<std::string>()));
		}
	}
	if (auto it = j.find("hotkeys"); it != j.end() && it->is_object()) {
		auto &h = s.hotkeys;
		read_hotkey(*it, "save", h.save);
		read_hotkey(*it, "save_long", h.save_long);
		read_hotkey(*it, "save_quick", h.save_quick);
		read_hotkey(*it, "bookmark", h.bookmark);
		read_hotkey(*it, "screenshot", h.screenshot);
		read_hotkey(*it, "record", h.record);
		read_hotkey(*it, "pause", h.pause);
		read_hotkey(*it, "push_to_talk", h.push_to_talk);
		read_hotkey(*it, "capture_game", h.capture_game);
		if (auto e = it->find("extra"); e != it->end() && e->is_array()) {
			h.extra.clear();
			for (const auto &item : *e) {
				SaveKey k;
				read(item, "name", k.name);
				read(item, "seconds", k.seconds);
				read_hotkey(item, "key", k.key);
				h.extra.push_back(k);
			}
		}
	}
	if (auto it = j.find("storage"); it != j.end() && it->is_object()) {
		auto &st = s.storage;
		read(*it, "clips_dir", st.clips_dir);
		read(*it, "buffer_dir", st.buffer_dir);
		read(*it, "name_template", st.name_template);
		read(*it, "per_game_folders", st.per_game_folders);
		read(*it, "auto_delete", st.auto_delete);
		read(*it, "max_gb", st.max_gb);
		read(*it, "keep_favorites", st.keep_favorites);
		read(*it, "share_cleanup_days", st.share_cleanup_days);
	}
	if (auto it = j.find("video"); it != j.end() && it->is_object()) {
		auto &v = s.video;
		read(*it, "preset", v.preset);
		read(*it, "resolution", v.resolution);
		read(*it, "fps", v.fps);
		read(*it, "encoder", v.encoder);
		read(*it, "rate_control", v.rate_control);
		read(*it, "quality", v.quality);
		read(*it, "bitrate_kbps", v.bitrate_kbps);
		read(*it, "keyint_sec", v.keyint_sec);
		read(*it, "nvenc_preset", v.nvenc_preset);
		read(*it, "lookahead", v.lookahead);
		read(*it, "psycho_visual", v.psycho_visual);
		read(*it, "container", v.container);
		read(*it, "hdr", v.hdr);
		read(*it, "scale_filter", v.scale_filter);
	}
	if (auto it = j.find("audio"); it != j.end() && it->is_object()) {
		auto &a = s.audio;
		if (auto t = it->find("tracks"); t != it->end() && t->is_array()) {
			a.tracks.clear();
			for (const auto &item : *t) {
				AudioTrack tr;
				read(item, "kind", tr.kind);
				read(item, "name", tr.name);
				read(item, "device", tr.device);
				read(item, "app_exe", tr.app_exe);
				read(item, "enabled", tr.enabled);
				read(item, "volume", tr.volume);
				a.tracks.push_back(tr);
			}
		}
		read(*it, "layout", a.layout);
		read(*it, "codec", a.codec);
		read(*it, "bitrate", a.bitrate);
		read(*it, "sample_rate", a.sample_rate);
		read(*it, "noise_suppression", a.noise_suppression);
		read(*it, "noise_gate", a.noise_gate);
		read(*it, "push_to_talk", a.push_to_talk);
		read(*it, "mute_mic_in_clips", a.mute_mic_in_clips);
	}
	if (auto it = j.find("overlay"); it != j.end() && it->is_object()) {
		auto &o = s.overlay;
		read(*it, "toast", o.toast);
		read(*it, "position", o.position);
		read(*it, "toast_seconds", o.toast_seconds);
		read(*it, "sound", o.sound);
		read(*it, "sound_volume", o.sound_volume);
		read(*it, "rec_indicator", o.rec_indicator);
		read(*it, "check_updates", o.check_updates);
		read(*it, "skip_version", o.skip_version);
	}
	if (auto it = j.find("share"); it != j.end() && it->is_object()) {
		auto &sh = s.share;
		read(*it, "target", sh.target);
		read(*it, "codec", sh.codec);
		read(*it, "discord_mb", sh.discord_mb);
		read(*it, "nitro_basic_mb", sh.nitro_basic_mb);
		read(*it, "nitro_mb", sh.nitro_mb);
		read(*it, "custom_mb", sh.custom_mb);
		read(*it, "copy_to_clipboard", sh.copy_to_clipboard);
		read(*it, "upload_link", sh.upload_link);
		read(*it, "r2_account", sh.r2_account);
		read(*it, "r2_access_key", sh.r2_access_key);
		std::string secret;
		read(*it, "r2_secret_dpapi", secret);
		sh.r2_secret = unprotect_secret(secret);
		read(*it, "r2_bucket", sh.r2_bucket);
		read(*it, "r2_public_url", sh.r2_public_url);
	}
}

/* ------------------------------------------------------------------ */

static void sanitize(Settings &s)
{
	auto clampi = [](int &v, int lo, int hi) { v = std::clamp(v, lo, hi); };
	clampi(s.clipping.clip_seconds, 5, 1800);
	clampi(s.clipping.post_seconds, 0, 60);
	clampi(s.clipping.long_seconds, 5, 1800);
	clampi(s.clipping.quick_seconds, 5, 1800);
	clampi(s.video.fps, 10, 240);
	clampi(s.video.quality, 1, 51);
	clampi(s.video.bitrate_kbps, 1000, 300000);
	clampi(s.video.keyint_sec, 1, 10);
	clampi(s.audio.bitrate, 64, 320);
	clampi(s.overlay.toast_seconds, 1, 10);
	clampi(s.overlay.sound_volume, 0, 100);
	clampi(s.storage.max_gb, 1, 100000);
	for (auto &e : s.hotkeys.extra)
		clampi(e.seconds, 5, 1800);
	for (auto &t : s.audio.tracks)
		clampi(t.volume, 0, 150);
	if (s.audio.sample_rate != 44100 && s.audio.sample_rate != 48000)
		s.audio.sample_rate = 48000;
	if (s.audio.tracks.size() > 5)
		s.audio.tracks.resize(5); /* OBS has 6 mixes: one for the mix, five separate */
}

Settings Settings::defaults()
{
	Settings s;
	s.hotkeys.save = Hotkey{VK_F8};
	s.hotkeys.save_long = Hotkey{VK_F8, false, true};
	s.hotkeys.save_quick = Hotkey{VK_F8, false, false, true};
	s.hotkeys.bookmark = Hotkey{VK_F9};
	s.hotkeys.screenshot = Hotkey{VK_F10};
	s.hotkeys.record = Hotkey{VK_F9, true};
	s.hotkeys.pause = Hotkey{VK_F8, true, true};
	s.hotkeys.push_to_talk = Hotkey{VK_XBUTTON1};
	s.hotkeys.capture_game = Hotkey{VK_F10, true};

	s.audio.tracks = {
		AudioTrack{"game", "Game", "default", "", true, 100},
		AudioTrack{"input", "Microphone", "default", "", true, 85},
		AudioTrack{"app", "Voice chat", "", "Discord.exe", true, 90},
		AudioTrack{"app", "Music", "", "Spotify.exe", false, 60},
	};
	return s;
}

Settings Settings::load()
{
	Settings s = defaults();
	std::ifstream f(paths::settings_file());
	if (!f)
		return s;
	try {
		json j = json::parse(f);
		from_json(j, s);
	} catch (const std::exception &e) {
		HL_WARN("settings.json is invalid, using defaults: %s", e.what());
		return defaults();
	}
	sanitize(s);
	return s;
}

bool Settings::save() const
{
	auto path = paths::settings_file();
	auto tmp = path;
	tmp += L".tmp";
	{
		std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
		if (!f)
			return false;
		f << to_json(*this).dump(2);
		if (!f)
			return false;
	}
	return MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING) != 0;
}

std::string Settings::clips_dir() const
{
	if (!storage.clips_dir.empty())
		return storage.clips_dir;
	return narrow(paths::default_clips_dir().wstring());
}

std::string Settings::buffer_dir() const
{
	if (!storage.buffer_dir.empty())
		return storage.buffer_dir;
	return narrow(paths::default_buffer_dir().wstring());
}

std::string Settings::share_dir() const
{
	return clips_dir() + "\\_Share";
}

int Settings::max_buffer_seconds() const
{
	int longest = std::max({clipping.clip_seconds, clipping.long_seconds, clipping.quick_seconds});
	for (const auto &e : hotkeys.extra)
		longest = std::max(longest, e.seconds);
	return longest;
}

void apply_video_preset(VideoSettings &v, const std::string &preset)
{
	v.preset = preset;
	if (preset == "light") {
		v.resolution = "1080p";
		v.fps = 30;
		v.bitrate_kbps = 10000;
		v.quality = 24;
	} else if (preset == "balanced") {
		v.resolution = "1080p";
		v.fps = 60;
		v.bitrate_kbps = 20000;
		v.quality = 22;
	} else if (preset == "high") {
		v.resolution = "1440p";
		v.fps = 60;
		v.bitrate_kbps = 40000;
		v.quality = 20;
	} else if (preset == "ultra") {
		v.resolution = "native";
		v.fps = 120;
		v.bitrate_kbps = 80000;
		v.quality = 18;
	}
}

} // namespace hl

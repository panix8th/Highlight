#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace hl {

struct Hotkey {
	uint32_t vk = 0; /* Windows virtual-key code, 0 = not set */
	bool ctrl = false;
	bool shift = false;
	bool alt = false;
	bool win = false;

	bool empty() const { return vk == 0; }
	bool operator==(const Hotkey &o) const
	{
		return vk == o.vk && ctrl == o.ctrl && shift == o.shift && alt == o.alt && win == o.win;
	}
	/* "Ctrl + Shift + F8" */
	std::string to_string() const;
};

std::string key_name(uint32_t vk);
uint32_t key_from_name(const std::string &name);

struct SaveKey {
	std::string name;
	Hotkey key;
	int seconds = 30;
};

struct ClippingSettings {
	int clip_seconds = 60;
	int post_seconds = 5;
	int long_seconds = 300;
	int quick_seconds = 15;

	std::string source = "game"; /* game | display | window */
	std::string display_id;      /* OBS monitor_id, empty = primary */
	std::string window;          /* OBS window spec "title:class:exe" */

	bool auto_start = true;  /* start buffer when a game is detected */
	bool idle_stop = true;   /* stop buffer when no game is running */
	bool capture_cursor = false;
	bool start_with_windows = true;
};

struct HotkeySettings {
	Hotkey save;
	Hotkey save_long;
	Hotkey save_quick;
	Hotkey bookmark;
	Hotkey screenshot;
	Hotkey record;
	Hotkey pause;
	Hotkey push_to_talk;
	std::vector<SaveKey> extra;
};

struct StorageSettings {
	std::string clips_dir;  /* empty = %USERPROFILE%\Videos\Highlight */
	std::string buffer_dir; /* empty = %LOCALAPPDATA%\Highlight\ring */
	std::string name_template = "{game}_{date}_{time}";
	bool per_game_folders = true;
	bool auto_delete = true;
	int max_gb = 100;
	bool keep_favorites = true;
	int share_cleanup_days = 7;
};

struct VideoSettings {
	std::string preset = "high";      /* light | balanced | high | ultra | custom */
	std::string resolution = "1440p"; /* native | 1440p | 1080p | 900p | 720p */
	int fps = 60;
	std::string encoder = "nvenc_hevc"; /* nvenc_av1 | nvenc_hevc | nvenc_h264 | amf_hevc | amf_h264 | qsv_hevc | qsv_h264 | x264 */
	std::string rate_control = "cq";    /* cq | vbr | cbr */
	int quality = 20;                   /* CQ level, lower = better */
	int bitrate_kbps = 40000;
	int keyint_sec = 1;
	std::string nvenc_preset = "p5";
	bool lookahead = true;
	bool psycho_visual = true;
	std::string container = "mp4"; /* mp4 | mkv | mov */
	bool hdr = false;
	std::string scale_filter = "lanczos"; /* lanczos | bicubic | bilinear */
};

struct AudioTrack {
	std::string kind = "game"; /* game | output | input | app */
	std::string name;
	std::string device = "default"; /* WASAPI device id for output/input */
	std::string app_exe;            /* for kind == app, e.g. "Discord.exe" */
	bool enabled = true;
	int volume = 100; /* percent, 0..150 */
};

struct AudioSettings {
	std::vector<AudioTrack> tracks;
	std::string layout = "both"; /* both | mixed | separate */
	std::string codec = "aac";   /* aac | opus */
	int bitrate = 192;
	int sample_rate = 48000;
	bool noise_suppression = true;
	bool noise_gate = true;
	bool push_to_talk = false;
	bool mute_mic_in_clips = false;
};

struct OverlaySettings {
	bool toast = true;
	std::string position = "top-left"; /* top-left | top-right | bottom-left | bottom-right */
	int toast_seconds = 3;
	bool sound = true;
	int sound_volume = 60;
	bool rec_indicator = false; /* off by default: keeps the screen clean */
	bool check_updates = true;  /* ask GitHub for a newer release when the window opens */
	std::string skip_version;   /* release the user chose to skip, e.g. "0.1.4" */
};

struct ShareSettings {
	std::string target = "discord"; /* discord | nitro_basic | nitro | best | custom */
	std::string codec = "h264";     /* h264 | hevc | av1 */
	int discord_mb = 10;
	int nitro_basic_mb = 50;
	int nitro_mb = 500;
	int custom_mb = 25;
	bool copy_to_clipboard = true;
};

struct Settings {
	ClippingSettings clipping;
	HotkeySettings hotkeys;
	StorageSettings storage;
	VideoSettings video;
	AudioSettings audio;
	OverlaySettings overlay;
	ShareSettings share;

	static Settings defaults();
	static Settings load(); /* never fails: falls back to defaults */
	bool save() const;

	std::string clips_dir() const;  /* resolved, absolute, UTF-8 */
	std::string buffer_dir() const; /* resolved, absolute, UTF-8 */
	std::string share_dir() const;  /* <clips>\_Share */

	/* Longest span the rolling buffer must hold, in seconds. */
	int max_buffer_seconds() const;
};

/* Applies a quality preset to the video settings. */
void apply_video_preset(VideoSettings &v, const std::string &preset);

} // namespace hl

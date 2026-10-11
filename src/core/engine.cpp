#include "engine.h"

#include "../common/games.h"
#include "../common/image.h"
#include "../common/log.h"
#include "../common/paths.h"
#include "../common/strings.h"
#include "../media/remux.h"

#include <graphics/graphics.h>
#include <util/base.h>
#include <util/platform.h>

#include <windows.h>

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <thread>
#include <vector>

namespace hl {

namespace fs = std::filesystem;

/* ------------------------------------------------------------------ */
/* helpers                                                            */

/* Set when game capture reports it cannot hook (anti-cheat, elevated game, refused
 * executable): a forced capture then switches to window capture right away. */
static std::atomic<bool> g_hook_failed{false};

static void obs_log_handler(int level, const char *format, va_list args, void *)
{
	char buf[4096];
	vsnprintf(buf, sizeof(buf), format, args);
	if (strstr(buf, "[game-capture") &&
	    (strstr(buf, "inject process failed") || strstr(buf, "could not open process") || strstr(buf, "blacklisted")))
		g_hook_failed = true;
	if (level > LOG_INFO)
		return; /* drop debug */
	log::Level l = level <= LOG_ERROR ? log::Error : level <= LOG_WARNING ? log::Warning : log::Info;
	log::write(l, "[obs] %s", buf);
}

static std::string encode_window_part(const std::string &s)
{
	std::string out;
	for (char c : s) {
		if (c == '#')
			out += "#22";
		else if (c == ':')
			out += "#3A";
		else
			out += c;
	}
	return out;
}

static std::string window_spec(const std::string &title, const std::string &cls, const std::string &exe)
{
	return encode_window_part(title) + ":" + encode_window_part(cls) + ":" + encode_window_part(exe);
}

static bool encoder_available(const char *id)
{
	const char *type = nullptr;
	for (size_t i = 0; obs_enum_encoder_types(i, &type); i++)
		if (type && strcmp(type, id) == 0)
			return true;
	return false;
}

struct EncoderChoice {
	const char *key;
	const char *id;
	const char *label;
	const char *family; /* nvenc | amf | qsv | x264 */
	const char *codec;  /* h264 | hevc | av1 */
};

static const EncoderChoice k_encoders[] = {
	{"nvenc_av1", "obs_nvenc_av1_tex", "NVENC AV1", "nvenc", "av1"},
	{"nvenc_hevc", "obs_nvenc_hevc_tex", "NVENC HEVC", "nvenc", "hevc"},
	{"nvenc_h264", "obs_nvenc_h264_tex", "NVENC H.264", "nvenc", "h264"},
	{"amf_hevc", "h265_texture_amf", "AMD HEVC", "amf", "hevc"},
	{"amf_h264", "h264_texture_amf", "AMD H.264", "amf", "h264"},
	{"qsv_hevc", "obs_qsv11_hevc", "Intel HEVC", "qsv", "hevc"},
	{"qsv_h264", "obs_qsv11_v2", "Intel H.264", "qsv", "h264"},
	{"x264", "obs_x264", "x264 (CPU)", "x264", "h264"},
};

static const EncoderChoice *pick_encoder(const std::string &wanted)
{
	for (const auto &e : k_encoders)
		if (wanted == e.key && encoder_available(e.id))
			return &e;
	static const char *fallback[] = {"nvenc_hevc", "nvenc_h264", "amf_hevc", "amf_h264",
					 "qsv_hevc",   "qsv_h264",   "x264"};
	for (const char *key : fallback)
		for (const auto &e : k_encoders)
			if (strcmp(e.key, key) == 0 && encoder_available(e.id))
				return &e;
	return nullptr;
}

/* Everything that requires tearing down and rebuilding the OBS pipeline. */
static std::string pipeline_key(const Settings &s)
{
	std::string k;
	auto add = [&](const std::string &v) {
		k += v;
		k += '\x1f';
	};
	add(s.clipping.source);
	add(s.clipping.display_id);
	add(s.clipping.window);
	add(std::to_string(s.clipping.capture_cursor));
	const auto &v = s.video;
	add(v.resolution);
	add(std::to_string(v.fps));
	add(v.encoder);
	add(v.rate_control);
	add(std::to_string(v.quality));
	add(std::to_string(v.bitrate_kbps));
	add(std::to_string(v.keyint_sec));
	add(v.nvenc_preset);
	add(std::to_string(v.lookahead));
	add(std::to_string(v.psycho_visual));
	add(std::to_string(v.hdr));
	add(v.scale_filter);
	const auto &a = s.audio;
	for (const auto &t : a.tracks) {
		add(t.kind);
		add(t.name);
		add(t.device);
		add(t.app_exe);
		add(std::to_string(t.enabled));
		add(std::to_string(t.volume));
	}
	add(a.layout);
	add(a.codec);
	add(std::to_string(a.bitrate));
	add(std::to_string(a.sample_rate));
	add(std::to_string(a.noise_suppression));
	add(std::to_string(a.noise_gate));
	add(s.buffer_dir());
	return k;
}

static int64_t keep_usec_for(const Settings &s)
{
	int post = s.clipping.post_seconds;
	return ((int64_t)s.max_buffer_seconds() + post + 10) * 1000000;
}

/* ------------------------------------------------------------------ */
/* libobs callbacks                                                   */

static void hooked_cb(void *data, calldata_t *cd)
{
	static_cast<Engine *>(data)->on_hooked(calldata_string(cd, "title"), calldata_string(cd, "class"),
					       calldata_string(cd, "executable"));
}

static void unhooked_cb(void *data, calldata_t *)
{
	static_cast<Engine *>(data)->on_unhooked();
}

static void rendered_cb(void *data)
{
	static_cast<Engine *>(data)->on_rendered();
}

static void record_stop_cb(void *data, calldata_t *cd)
{
	static_cast<Engine *>(data)->on_recording_stopped((int)calldata_int(cd, "code"));
}

/* ------------------------------------------------------------------ */

Engine::Engine() = default;

Engine::~Engine()
{
	shutdown();
}

bool Engine::init(const Settings &settings, Events events, std::string *err)
{
	settings_ = settings;
	events_ = std::move(events);

	base_set_log_handler(obs_log_handler, nullptr);

	std::string module_config = narrow((paths::app_data() / L"obs").wstring());
	if (!obs_startup("en-US", module_config.c_str(), nullptr)) {
		if (err)
			*err = "libobs failed to start";
		return false;
	}
	started_ = true;

	/* Video must exist before plugins load: win-capture checks the graphics device. */
	if (!reset_audio()) {
		if (err)
			*err = "Could not start audio";
		return false;
	}
	if (!reset_video(err))
		return false;

	load_modules();
	RingOutput::register_type();

	obs_add_main_rendered_callback(rendered_cb, this);
	rendered_cb_ = true;

	if (!build_capture(err))
		return false;
	build_audio();
	if (!build_encoders(err))
		return false;
	if (!build_outputs(err))
		return false;
	return true;
}

void Engine::shutdown()
{
	if (!started_)
		return;

	if (recording_active_ && record_) {
		obs_output_force_stop(record_);
		recording_active_ = false;
	}
	stop_buffer();

	if (rendered_cb_) {
		obs_remove_main_rendered_callback(rendered_cb, this);
		rendered_cb_ = false;
	}
	if (stage_) {
		obs_enter_graphics();
		gs_stagesurface_destroy((gs_stagesurf_t *)stage_);
		obs_leave_graphics();
		stage_ = nullptr;
	}

	if (shot_thread_.joinable())
		shot_thread_.join();

	destroy_outputs();
	destroy_encoders();
	destroy_audio();
	destroy_capture();

	obs_shutdown();
	started_ = false;
	HL_INFO("libobs shut down");
}

void Engine::load_modules()
{
	if (modules_loaded_)
		return;
	modules_loaded_ = true;
	static const char *modules[] = {"win-capture", "win-wasapi",  "obs-nvenc", "obs-ffmpeg",
					"obs-filters", "obs-x264",    "obs-qsv11"};
	for (const char *m : modules) {
		std::string bin = std::string("../../obs-plugins/64bit/") + m + ".dll";
		std::string data = std::string("../../data/obs-plugins/") + m;
		obs_module_t *mod = nullptr;
		int r = obs_open_module(&mod, bin.c_str(), data.c_str());
		if (r != 0) {
			HL_WARN("Could not open module %s (code %d)", m, r);
			continue;
		}
		if (!obs_init_module(mod))
			HL_WARN("Could not initialize module %s", m);
	}
	obs_post_load_modules();
}

bool Engine::reset_audio()
{
	obs_audio_info oai = {};
	oai.samples_per_sec = (uint32_t)settings_.audio.sample_rate;
	oai.speakers = SPEAKERS_STEREO;
	return obs_reset_audio(&oai);
}

bool Engine::reset_video(std::string *err)
{
	HMONITOR mon = MonitorFromPoint(POINT{0, 0}, MONITOR_DEFAULTTOPRIMARY);
	MONITORINFO mi = {sizeof(mi)};
	if (GetMonitorInfoW(mon, &mi)) {
		base_w_ = (mi.rcMonitor.right - mi.rcMonitor.left) & ~1;
		base_h_ = (mi.rcMonitor.bottom - mi.rcMonitor.top) & ~1;
	}

	const auto &v = settings_.video;
	int target_h = base_h_;
	if (v.resolution == "1440p")
		target_h = 1440;
	else if (v.resolution == "1080p")
		target_h = 1080;
	else if (v.resolution == "900p")
		target_h = 900;
	else if (v.resolution == "720p")
		target_h = 720;

	if (target_h >= base_h_) {
		out_w_ = base_w_;
		out_h_ = base_h_;
	} else {
		out_h_ = target_h & ~1;
		out_w_ = (int)llround((double)base_w_ * target_h / base_h_) & ~1;
	}

	bool hdr = v.hdr && v.encoder.find("h264") == std::string::npos && v.encoder != "x264";

	obs_video_info ovi = {};
	ovi.graphics_module = "libobs-d3d11";
	ovi.fps_num = (uint32_t)v.fps;
	ovi.fps_den = 1;
	ovi.base_width = (uint32_t)base_w_;
	ovi.base_height = (uint32_t)base_h_;
	ovi.output_width = (uint32_t)out_w_;
	ovi.output_height = (uint32_t)out_h_;
	ovi.output_format = hdr ? VIDEO_FORMAT_P010 : VIDEO_FORMAT_NV12;
	ovi.adapter = 0;
	ovi.gpu_conversion = true;
	ovi.colorspace = hdr ? VIDEO_CS_2100_PQ : VIDEO_CS_709;
	ovi.range = VIDEO_RANGE_PARTIAL;
	ovi.scale_type = v.scale_filter == "bilinear"  ? OBS_SCALE_BILINEAR
			 : v.scale_filter == "bicubic" ? OBS_SCALE_BICUBIC
						       : OBS_SCALE_LANCZOS;

	int r = obs_reset_video(&ovi);
	if (r != OBS_VIDEO_SUCCESS) {
		if (err)
			*err = r == OBS_VIDEO_NOT_SUPPORTED ? "Your graphics card does not support DirectX 11"
							    : "Could not start video (code " + std::to_string(r) + ")";
		return false;
	}
	HL_INFO("Video: canvas %dx%d, output %dx%d @ %d fps%s", base_w_, base_h_, out_w_, out_h_, v.fps,
		hdr ? " HDR" : "");
	return true;
}

static std::string primary_monitor_id()
{
	std::string best;
	obs_properties_t *props = obs_get_source_properties("monitor_capture");
	if (!props)
		return best;
	obs_property_t *p = obs_properties_get(props, "monitor_id");
	size_t n = p ? obs_property_list_item_count(p) : 0;
	for (size_t i = 0; i < n; i++) {
		const char *name = obs_property_list_item_name(p, i);
		const char *val = obs_property_list_item_string(p, i);
		if (!val)
			continue;
		if (best.empty())
			best = val;
		if (name && strstr(name, "Primary")) {
			best = val;
			break;
		}
	}
	obs_properties_destroy(props);
	return best;
}

bool Engine::build_capture(std::string *err)
{
	const auto &c = settings_.clipping;
	obs_data_t *s = obs_data_create();
	const char *id = "game_capture";

	if (c.source == "display") {
		id = "monitor_capture";
		std::string mon = c.display_id.empty() ? primary_monitor_id() : c.display_id;
		obs_data_set_string(s, "monitor_id", mon.c_str());
		obs_data_set_int(s, "method", 0);
		obs_data_set_bool(s, "capture_cursor", c.capture_cursor);
	} else if (c.source == "window") {
		id = "window_capture";
		obs_data_set_string(s, "window", c.window.c_str());
		obs_data_set_int(s, "method", 0);
		obs_data_set_int(s, "priority", 2);
		obs_data_set_bool(s, "cursor", c.capture_cursor);
	} else {
		obs_data_set_string(s, "capture_mode", "any_fullscreen");
		obs_data_set_bool(s, "capture_cursor", c.capture_cursor);
		obs_data_set_bool(s, "anti_cheat_hook", true);
		obs_data_set_bool(s, "allow_transparency", false);
		obs_data_set_bool(s, "limit_framerate", false);
		obs_data_set_bool(s, "capture_overlays", false);
	}

	capture_ = obs_source_create(id, "Highlight Capture", s, nullptr);
	obs_data_release(s);
	if (!capture_) {
		if (err)
			*err = std::string("Could not create ") + id;
		return false;
	}

	scene_ = obs_scene_create_private("Highlight");
	capture_item_ = add_to_scene(capture_);
	obs_set_output_source(0, obs_scene_get_source(scene_));

	if (strcmp(id, "game_capture") == 0) {
		signal_handler_t *sh = obs_source_get_signal_handler(capture_);
		signal_handler_connect(sh, "hooked", hooked_cb, this);
		signal_handler_connect(sh, "unhooked", unhooked_cb, this);
	} else {
		/* name clips after the captured app, not "Window" */
		std::string name = c.source == "display" ? "Desktop" : "Window";
		if (c.source == "window") {
			/* OBS window spec "title:class:exe"; ':' inside a part is written as "#3A" */
			std::vector<std::string> parts;
			size_t start = 0;
			for (size_t i = 0; i <= c.window.size(); i++)
				if (i == c.window.size() || c.window[i] == ':') {
					std::string part = c.window.substr(start, i - start);
					for (size_t k; (k = part.find("#3A")) != std::string::npos;)
						part.replace(k, 3, ":");
					parts.push_back(part);
					start = i + 1;
				}
			if (parts.size() == 3 && !(parts[0].empty() && parts[2].empty())) {
				std::string n = game_display_name(parts[2], parts[0]);
				if (!n.empty())
					name = n;
			}
		}
		std::lock_guard lock(game_mutex_);
		game_ = name;
	}
	return true;
}

void Engine::destroy_capture()
{
	stop_fallback(false);
	forced_hwnd_ = nullptr;
	forced_spec_.clear();
	forced_name_.clear();
	game_hooked_ = false; /* the hooked/unhooked signals are disconnected below */
	{
		std::lock_guard lock(game_mutex_);
		game_window_spec_.clear();
	}
	capture_item_ = nullptr;
	obs_set_output_source(0, nullptr);
	if (capture_) {
		signal_handler_t *sh = obs_source_get_signal_handler(capture_);
		signal_handler_disconnect(sh, "hooked", hooked_cb, this);
		signal_handler_disconnect(sh, "unhooked", unhooked_cb, this);
		obs_source_release(capture_);
		capture_ = nullptr;
	}
	if (scene_) {
		obs_scene_release(scene_);
		scene_ = nullptr;
	}
}

bool Engine::build_audio()
{
	const auto &a = settings_.audio;
	const bool game_mode = settings_.clipping.source == "game";

	std::vector<int> enabled;
	for (int i = 0; i < (int)a.tracks.size(); i++)
		if (a.tracks[i].enabled && enabled.size() < 5)
			enabled.push_back(i);

	track_titles_.clear();
	if (a.layout == "both") {
		track_titles_.push_back("Mix");
		for (int i : enabled)
			track_titles_.push_back(a.tracks[i].name);
	} else if (a.layout == "separate" && !enabled.empty()) {
		for (int i : enabled)
			track_titles_.push_back(a.tracks[i].name);
	} else {
		track_titles_.push_back("Mix");
	}

	uint32_t channel = 1;
	for (size_t n = 0; n < enabled.size(); n++) {
		const AudioTrack &t = a.tracks[enabled[n]];
		obs_data_t *s = obs_data_create();
		const char *id = nullptr;
		bool is_game_process = false;

		if (t.kind == "game" && game_mode) {
			id = "wasapi_process_output_capture";
			std::lock_guard lock(game_mutex_);
			obs_data_set_string(s, "window", game_window_spec_.c_str());
			obs_data_set_int(s, "priority", 2);
			is_game_process = true;
		} else if (t.kind == "game" || t.kind == "output") {
			id = "wasapi_output_capture";
			obs_data_set_string(s, "device_id", t.device.empty() ? "default" : t.device.c_str());
		} else if (t.kind == "input") {
			id = "wasapi_input_capture";
			obs_data_set_string(s, "device_id", t.device.empty() ? "default" : t.device.c_str());
		} else if (t.kind == "app") {
			id = "wasapi_process_output_capture";
			obs_data_set_string(s, "window", window_spec("", "", t.app_exe).c_str());
			obs_data_set_int(s, "priority", 2);
		}

		obs_source_t *src = id ? obs_source_create(id, t.name.c_str(), s, nullptr) : nullptr;
		obs_data_release(s);
		if (!src) {
			HL_WARN("Could not create audio source '%s'", t.name.c_str());
			continue;
		}

		obs_source_set_volume(src, (float)t.volume / 100.0f);

		uint32_t mixers;
		if (a.layout == "both")
			mixers = 1u | (1u << (n + 1));
		else if (a.layout == "separate")
			mixers = 1u << n;
		else
			mixers = 1u;
		obs_source_set_audio_mixers(src, mixers);
		obs_set_output_source(channel++, src);

		if (is_game_process)
			game_audio_ = src;

		if (t.kind == "input" && !mic_) {
			mic_ = src;
			if (a.noise_suppression) {
				obs_data_t *fs = obs_data_create();
				obs_data_set_string(fs, "method", "rnnoise");
				obs_source_t *f = obs_source_create_private("noise_suppress_filter", "Noise suppression", fs);
				if (f) {
					obs_source_filter_add(src, f);
					obs_source_release(f);
				}
				obs_data_release(fs);
			}
			if (a.noise_gate) {
				obs_data_t *fs = obs_data_create();
				obs_data_set_double(fs, "close_threshold", -45.0);
				obs_data_set_double(fs, "open_threshold", -40.0);
				obs_source_t *f = obs_source_create_private("noise_gate_filter", "Noise gate", fs);
				if (f) {
					obs_source_filter_add(src, f);
					obs_source_release(f);
				}
				obs_data_release(fs);
			}
		}

		audio_sources_.push_back(src);
		audio_track_index_.push_back(enabled[n]);
	}

	apply_mic_state();
	return true;
}

void Engine::destroy_audio()
{
	for (uint32_t ch = 1; ch <= (uint32_t)audio_sources_.size(); ch++)
		obs_set_output_source(ch, nullptr);
	for (auto *s : audio_sources_)
		obs_source_release(s);
	audio_sources_.clear();
	audio_track_index_.clear();
	mic_ = nullptr;
	game_audio_ = nullptr;
}

void Engine::apply_mic_state()
{
	if (!mic_)
		return;
	const auto &a = settings_.audio;
	bool muted = a.mute_mic_in_clips || (a.push_to_talk && !ptt_down_);
	obs_source_set_muted(mic_, muted);
}

void Engine::set_push_to_talk(bool down)
{
	ptt_down_ = down;
	apply_mic_state();
}

bool Engine::build_encoders(std::string *err)
{
	const auto &v = settings_.video;
	const EncoderChoice *choice = pick_encoder(v.encoder);
	if (!choice) {
		if (err)
			*err = "No video encoder is available";
		return false;
	}

	const bool hdr = v.hdr && strcmp(choice->codec, "h264") != 0;
	const std::string family = choice->family;
	obs_data_t *s = obs_data_create();
	obs_data_set_int(s, "keyint_sec", v.keyint_sec);
	obs_data_set_int(s, "bitrate", v.bitrate_kbps);

	if (family == "nvenc") {
		const bool av1 = strcmp(choice->codec, "av1") == 0;
		if (v.rate_control == "cbr") {
			obs_data_set_string(s, "rate_control", "CBR");
		} else if (v.rate_control == "vbr") {
			obs_data_set_string(s, "rate_control", "VBR");
			obs_data_set_int(s, "max_bitrate", v.bitrate_kbps * 3 / 2);
		} else {
			obs_data_set_string(s, "rate_control", "CQVBR");
			int q = av1 ? std::min(63, (int)llround(v.quality * 1.5)) : v.quality;
			obs_data_set_int(s, "target_quality", q);
			obs_data_set_int(s, "max_bitrate", v.bitrate_kbps * 3 / 2);
		}
		obs_data_set_string(s, "preset", v.nvenc_preset.c_str());
		obs_data_set_string(s, "tune", "hq");
		obs_data_set_string(s, "multipass", "qres");
		obs_data_set_string(s, "profile", strcmp(choice->codec, "h264") == 0 ? "high" : (hdr ? "main10" : "main"));
		obs_data_set_bool(s, "lookahead", v.lookahead);
		obs_data_set_bool(s, "adaptive_quantization", v.psycho_visual);
		obs_data_set_int(s, "bf", 2);
	} else if (family == "amf") {
		obs_data_set_string(s, "rate_control", v.rate_control == "cbr" ? "CBR" : v.rate_control == "vbr" ? "VBR" : "QVBR");
		obs_data_set_int(s, "cqp", v.quality);
		obs_data_set_string(s, "preset", "quality");
		obs_data_set_string(s, "profile", strcmp(choice->codec, "h264") == 0 ? "high" : "main");
	} else if (family == "qsv") {
		obs_data_set_string(s, "rate_control", v.rate_control == "cbr" ? "CBR" : v.rate_control == "vbr" ? "VBR" : "ICQ");
		obs_data_set_int(s, "icq_quality", v.quality);
		obs_data_set_string(s, "target_usage", "TU4");
		obs_data_set_string(s, "profile", strcmp(choice->codec, "h264") == 0 ? "high" : "main");
	} else {
		obs_data_set_string(s, "rate_control", v.rate_control == "cbr" ? "CBR" : v.rate_control == "vbr" ? "VBR" : "CRF");
		obs_data_set_int(s, "crf", v.quality + 3);
		obs_data_set_string(s, "preset", "veryfast");
		obs_data_set_string(s, "profile", "high");
	}

	venc_ = obs_video_encoder_create(choice->id, "Highlight Video", s, nullptr);
	obs_data_release(s);
	if (!venc_) {
		if (err)
			*err = std::string("Could not create the ") + choice->label + " encoder";
		return false;
	}
	obs_encoder_set_video(venc_, obs_get_video());
	encoder_label_ = choice->label;
	if (v.encoder != choice->key)
		HL_WARN("Encoder %s is not available, using %s", v.encoder.c_str(), choice->label);
	HL_INFO("Video encoder: %s", choice->label);

	const auto &a = settings_.audio;
	const char *aid = a.codec == "opus" ? "ffmpeg_opus" : "ffmpeg_aac";
	for (size_t m = 0; m < track_titles_.size() && m < MAX_AUDIO_MIXES; m++) {
		obs_data_t *as = obs_data_create();
		obs_data_set_int(as, "bitrate", a.bitrate);
		std::string name = "Highlight Audio " + std::to_string(m + 1);
		obs_encoder_t *enc = obs_audio_encoder_create(aid, name.c_str(), as, m, nullptr);
		obs_data_release(as);
		if (!enc) {
			HL_WARN("Could not create audio encoder %zu", m);
			continue;
		}
		obs_encoder_set_audio(enc, obs_get_audio());
		aencs_.push_back(enc);
	}
	return true;
}

void Engine::destroy_encoders()
{
	for (auto *e : aencs_)
		obs_encoder_release(e);
	aencs_.clear();
	if (venc_) {
		obs_encoder_release(venc_);
		venc_ = nullptr;
	}
}

bool Engine::build_outputs(std::string *err)
{
	ring_ = std::make_unique<RingOutput>();
	if (!ring_->create("Highlight Buffer")) {
		if (err)
			*err = "Could not create the rolling buffer";
		return false;
	}
	ring_->set_track_titles(track_titles_);
	ring_->set_hdr(settings_.video.hdr && strcmp(obs_encoder_get_codec(venc_), "h264") != 0);
	ring_->set_done_callback([this](const ClipResult &r) {
		if (events_.clip_done)
			events_.clip_done(r);
	});

	obs_output_set_video_encoder(ring_->output(), venc_);
	for (size_t i = 0; i < aencs_.size(); i++)
		obs_output_set_audio_encoder(ring_->output(), aencs_[i], i);
	return true;
}

void Engine::destroy_outputs()
{
	if (record_) {
		signal_handler_disconnect(obs_output_get_signal_handler(record_), "stop", record_stop_cb, this);
		obs_output_release(record_);
		record_ = nullptr;
	}
	ring_.reset();
}

bool Engine::start_buffer(std::string *err)
{
	if (!ring_)
		return false;
	if (ring_->active())
		return true;

	std::string e;
	if (!ring_->open_ring(widen(settings_.buffer_dir()), keep_usec_for(settings_), &e)) {
		if (err)
			*err = e;
		return false;
	}
	if (!obs_output_start(ring_->output())) {
		const char *last = obs_output_get_last_error(ring_->output());
		if (err)
			*err = last && *last ? last : "The encoder could not start";
		ring_->close_ring();
		return false;
	}
	HL_INFO("Buffer started");
	return true;
}

void Engine::stop_buffer()
{
	if (!ring_ || !ring_->active())
		return;
	obs_output_stop(ring_->output());
	/* Clips still being written read from the buffer: let them finish first. */
	ring_->wait_for_saves(120000);
	ring_->close_ring();
	HL_INFO("Buffer stopped");
}

bool Engine::buffer_active() const
{
	return ring_ && ring_->active();
}

bool Engine::apply(const Settings &settings, std::string *err, bool force)
{
	if (!started_) {
		if (err)
			*err = "Restart Highlight to apply these settings";
		return false;
	}
	const bool rebuild = force || pipeline_key(settings) != pipeline_key(settings_) ||
			     settings.audio.sample_rate != settings_.audio.sample_rate;
	const bool sample_rate_changed = settings.audio.sample_rate != settings_.audio.sample_rate;
	settings_ = settings;

	if (!rebuild) {
		if (ring_)
			ring_->set_keep(keep_usec_for(settings_));
		apply_mic_state();
		return true;
	}

	if (recording_active_) {
		if (err)
			*err = "Video and audio changes apply after the recording stops";
		return false;
	}

	const bool was_active = buffer_active();
	/* The capture source is rebuilt: keep a forced window, and tell the app when the
	 * game it was capturing is no longer captured (the new source hooks it again). */
	const HWND keep_forced = forced_hwnd_;
	const bool had_game = game_hooked_ || fallback_on_;
	auto game_gone = [&]() {
		if (had_game && events_.game_unhooked)
			events_.game_unhooked();
	};
	stop_buffer();
	destroy_outputs();
	destroy_encoders();
	destroy_audio();
	destroy_capture();

	if (sample_rate_changed && !reset_audio())
		HL_WARN("Could not change the audio sample rate");
	if (!reset_video(err)) {
		game_gone();
		return false;
	}
	load_modules();
	RingOutput::register_type();
	if (!rendered_cb_) {
		obs_add_main_rendered_callback(rendered_cb, this);
		rendered_cb_ = true;
	}
	if (!build_capture(err)) {
		game_gone();
		return false;
	}
	build_audio();
	if (keep_forced && IsWindow(keep_forced) && settings_.clipping.source == "game" &&
	    capture_window(keep_forced, nullptr, nullptr, nullptr)) {
		/* still forced: hooks again or falls back to window capture within seconds */
	} else {
		game_gone();
	}
	if (!build_encoders(err))
		return false;
	if (!build_outputs(err))
		return false;
	if (was_active)
		return start_buffer(err);
	return true;
}

std::string Engine::game() const
{
	std::lock_guard lock(game_mutex_);
	return game_;
}

void Engine::on_hooked(const char *title, const char *cls, const char *exe)
{
	game_hooked_ = true;
	std::string name = game_display_name(exe ? exe : "", title ? title : "");
	std::string spec = window_spec(title ? title : "", cls ? cls : "", exe ? exe : "");
	{
		std::lock_guard lock(game_mutex_);
		game_ = name;
		game_window_spec_ = spec;
	}
	target_game_audio(spec);
	HL_INFO("Game detected: %s (%s)", name.c_str(), exe ? exe : "");
	if (events_.game_hooked)
		events_.game_hooked(name);
}

void Engine::on_unhooked()
{
	game_hooked_ = false;
	if (fallback_on_)
		return; /* retargeting game capture unhooks it; window capture still has the game */
	HL_INFO("Game closed");
	if (events_.game_unhooked)
		events_.game_unhooked();
}

/* ------------------------------------------------------------------ */
/* forced capture: "capture the game in front"                        */

/* "priority" for finding the window (libobs window_priority): CLASS 0, TITLE 1, EXE 2.
 * EXE survives title changes (map names, FPS counters). */
static constexpr int k_priority_class = 0;
static constexpr int k_priority_exe = 2;
/* window_capture "method": Windows Graphics Capture, which works for modern
 * games (D3D11/12, Vulkan) that BitBlt cannot see. */
static constexpr int k_method_wgc = 2;
/* How long the game capture hook gets before falling back to window capture. */
static constexpr uint64_t k_hook_timeout_ms = 8000; /* the anti-cheat inject helper alone takes ~4 s */

obs_sceneitem_t *Engine::add_to_scene(obs_source_t *src)
{
	obs_sceneitem_t *item = scene_ && src ? obs_scene_add(scene_, src) : nullptr;
	if (item) {
		vec2 bounds;
		bounds.x = (float)base_w_;
		bounds.y = (float)base_h_;
		obs_sceneitem_set_bounds_type(item, OBS_BOUNDS_SCALE_INNER);
		obs_sceneitem_set_bounds_alignment(item, 0 /* OBS_ALIGN_CENTER */);
		obs_sceneitem_set_bounds(item, &bounds);
	}
	return item;
}

void Engine::target_game_audio(const std::string &spec)
{
	if (!game_audio_ || spec.empty())
		return;
	obs_data_t *s = obs_data_create();
	obs_data_set_string(s, "window", spec.c_str());
	obs_data_set_int(s, "priority", k_priority_exe);
	obs_source_update(game_audio_, s);
	obs_data_release(s);
}

void Engine::set_game_capture_target(const std::string &spec, bool idle)
{
	const int priority = forced_priority_;
	if (!capture_ || strcmp(obs_source_get_id(capture_), "game_capture") != 0)
		return;
	obs_data_t *s = obs_data_create();
	if (idle) {
		/* "hotkey" mode hooks only when its own hotkey is pressed, which Highlight never
		 * binds: game capture stops retrying (and logging) a window it cannot hook */
		obs_data_set_string(s, "capture_mode", "hotkey");
	} else if (spec.empty()) {
		obs_data_set_string(s, "capture_mode", "any_fullscreen");
		obs_data_set_int(s, "hook_rate", 1); /* normal: retry every 2 s */
	} else {
		/* priority EXE: the title may change (map names, FPS counters) */
		obs_data_set_string(s, "capture_mode", "window");
		obs_data_set_string(s, "window", spec.c_str());
		obs_data_set_int(s, "priority", priority);
		obs_data_set_int(s, "hook_rate", 3); /* fastest: retry every 0.2 s, so the fallback timer is fair */
	}
	obs_source_update(capture_, s);
	obs_data_release(s);
}

bool Engine::window_info(HWND hwnd, std::string *title, std::string *cls, std::string *exe)
{
	if (!hwnd || !IsWindow(hwnd))
		return false;
	wchar_t t[512] = {}, c[256] = {};
	GetWindowTextW(hwnd, t, 512);
	GetClassNameW(hwnd, c, 256);
	DWORD pid = 0;
	GetWindowThreadProcessId(hwnd, &pid);
	std::string e;
	if (HANDLE proc = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid)) {
		wchar_t path[MAX_PATH] = {};
		DWORD size = MAX_PATH;
		if (QueryFullProcessImageNameW(proc, 0, path, &size))
			e = narrow(fs::path(path).filename().wstring());
		CloseHandle(proc);
	}
	if (title)
		*title = narrow(t);
	if (cls)
		*cls = narrow(c);
	if (exe)
		*exe = e;
	return !e.empty();
}

/* Executables OBS game capture refuses to hook (plugins/win-capture/game-capture.c,
 * blacklisted_exes in OBS 32.2.2). */
bool Engine::game_capture_refuses(const std::string &exe)
{
	static const char *const list[] = {"explorer",       "steam",       "battle.net",
					   "galaxyclient",   "skype",       "uplay",
					   "origin",         "devenv",      "taskmgr",
					   "chrome",         "discord",     "firefox",
					   "systemsettings", "applicationframehost", "cmd",
					   "shellexperiencehost", "winstore.app", "searchui",
					   "lockapp",        "windowsinternal.composableshell.experiences.textinput.inputapp"};
	std::string base = to_lower(exe);
	if (base.size() > 4 && base.substr(base.size() - 4) == ".exe")
		base.resize(base.size() - 4);
	for (const char *e : list)
		if (base == e)
			return true;
	return false;
}

bool Engine::capture_window(HWND hwnd, std::string *name_out, std::string *exe_out, std::string *err)
{
	auto fail = [&](const std::string &m) {
		if (err)
			*err = m;
		return false;
	};
	if (!started_ || !capture_ || !scene_)
		return fail("Capture is not running");
	if (settings_.clipping.source != "game")
		return fail("Set \"What to capture\" to Game first");

	std::string title, cls, exe;
	window_info(hwnd, &title, &cls, &exe);
	if (cls.empty())
		return fail("Highlight cannot read this window");
	/* OBS names an unreadable executable "unknown"; match such a window by class
	 * and exact title instead (priority CLASS) */
	const bool exe_known = !exe.empty();
	if (!exe_known)
		exe = "unknown";
	const std::string spec = window_spec(title, cls, exe);
	const std::string name = game_display_name(exe, title);
	if (name_out)
		*name_out = name;
	if (exe_out)
		*exe_out = exe;

	/* Already capturing this game (found by itself or forced earlier): nothing to change. */
	{
		std::lock_guard lock(game_mutex_);
		const size_t colon = game_window_spec_.find_last_of(':');
		const std::string hooked_exe = colon == std::string::npos ? "" : game_window_spec_.substr(colon + 1);
		if (game_hooked_ && !hooked_exe.empty() && to_lower(hooked_exe) == to_lower(encode_window_part(exe)))
			return true;
	}
	if (forced_hwnd_ == hwnd && fallback_)
		return true;

	/* no "game gone" in between: the new window is captured within seconds */
	stop_fallback(false);
	forced_hwnd_ = hwnd;
	forced_spec_ = spec;
	forced_name_ = name;
	forced_priority_ = exe_known ? k_priority_exe : k_priority_class;
	forced_since_ms_ = GetTickCount64();
	forced_hidden_ticks_ = 0;
	g_hook_failed = false;
	HL_INFO("Forced capture of %s (%s)", name.c_str(), exe.c_str());
	if (game_capture_refuses(exe)) {
		/* game capture never hooks these: use window capture right away */
		set_game_capture_target(spec, true);
		start_fallback();
	} else {
		set_game_capture_target(spec);
	}
	return true;
}

void Engine::start_fallback()
{
	if (fallback_ || !scene_ || forced_spec_.empty())
		return;
	obs_data_t *s = obs_data_create();
	obs_data_set_string(s, "window", forced_spec_.c_str());
	obs_data_set_int(s, "priority", forced_priority_);
	obs_data_set_int(s, "method", k_method_wgc);
	obs_data_set_bool(s, "cursor", settings_.clipping.capture_cursor);
	obs_data_set_bool(s, "client_area", true);
	fallback_ = obs_source_create("window_capture", "Highlight Window Capture", s, nullptr);
	obs_data_release(s);
	if (!fallback_) {
		HL_WARN("Window capture fallback could not be created");
		return;
	}
	/* Added on top of game capture, which stays in the scene and keeps trying to
	 * hook (a source that is not showing stops capturing); tick() drops this
	 * fallback if the hook takes later. */
	fallback_item_ = add_to_scene(fallback_);
	fallback_on_ = true;
	target_game_audio(forced_spec_);
	{
		std::lock_guard lock(game_mutex_);
		game_ = forced_name_;
		game_window_spec_ = forced_spec_;
	}
	HL_INFO("Game capture did not hook %s: using window capture", forced_name_.c_str());
	if (events_.game_hooked)
		events_.game_hooked(forced_name_);
}

void Engine::stop_fallback(bool notify)
{
	if (fallback_item_) {
		obs_sceneitem_remove(fallback_item_);
		fallback_item_ = nullptr;
	}
	if (fallback_) {
		obs_source_release(fallback_);
		fallback_ = nullptr;
	}
	const bool was_on = fallback_on_.exchange(false);
	if (was_on && notify && !game_hooked_ && events_.game_unhooked)
		events_.game_unhooked();
}

void Engine::capture_auto()
{
	if (!forced_hwnd_ && !fallback_)
		return;
	stop_fallback();
	forced_hwnd_ = nullptr;
	forced_spec_.clear();
	forced_name_.clear();
	set_game_capture_target("");
	HL_INFO("Back to automatic game detection");
}

void Engine::tick()
{
	if (!forced_hwnd_)
		return;
	/* Hidden for a few seconds counts as closed (games that hide their window on
	 * exit, launchers that go to the tray). Minimized windows stay visible. */
	forced_hidden_ticks_ = IsWindowVisible(forced_hwnd_) ? 0 : forced_hidden_ticks_ + 1;
	if (!IsWindow(forced_hwnd_) || forced_hidden_ticks_ >= 5) {
		HL_INFO("Captured window closed");
		capture_auto();
		return;
	}
	if (game_hooked_ && fallback_) {
		/* the hook took after all: prefer game capture */
		HL_INFO("Game capture hooked %s, leaving window capture", forced_name_.c_str());
		stop_fallback();
		return;
	}
	if (game_hooked_ || fallback_)
		return;
	if (g_hook_failed.exchange(false)) {
		/* injection refused (anti-cheat, elevated game): stop retrying, use window capture */
		set_game_capture_target(forced_spec_, true);
		start_fallback();
	} else if (GetTickCount64() - forced_since_ms_ > k_hook_timeout_ms) {
		/* slow to hook: window capture now, game capture keeps trying and takes over */
		start_fallback();
	}
}

std::string Engine::container_format() const
{
	if (settings_.video.container == "mkv")
		return "matroska";
	if (settings_.video.container == "mov")
		return "mov";
	return "mp4";
}

std::string Engine::container_ext() const
{
	if (settings_.video.container == "mkv")
		return "mkv";
	if (settings_.video.container == "mov")
		return "mov";
	return "mp4";
}

std::string Engine::make_path(const std::string &suffix, const std::string &ext) const
{
	std::string game = this->game();
	if (game.empty())
		game = "Desktop";

	std::string dir = settings_.clips_dir();
	if (settings_.storage.per_game_folders)
		dir += "\\" + sanitize_filename(game);
	std::error_code ec;
	fs::create_directories(widen(dir), ec);

	SYSTEMTIME t;
	GetLocalTime(&t);
	char date[16], time[16];
	snprintf(date, sizeof(date), "%04d-%02d-%02d", t.wYear, t.wMonth, t.wDay);
	snprintf(time, sizeof(time), "%02d-%02d-%02d", t.wHour, t.wMinute, t.wSecond);

	std::string name = settings_.storage.name_template.empty() ? "{game}_{date}_{time}"
								    : settings_.storage.name_template;
	auto replace = [&](const std::string &key, const std::string &val) {
		for (size_t pos; (pos = name.find(key)) != std::string::npos;)
			name.replace(pos, key.size(), val);
	};
	replace("{game}", game);
	replace("{date}", date);
	replace("{time}", time);
	name = sanitize_filename(name) + suffix;

	std::string path = dir + "\\" + name + "." + ext;
	for (int i = 2; fs::exists(widen(path), ec) && i < 1000; i++)
		path = dir + "\\" + name + " (" + std::to_string(i) + ")." + ext;
	return path;
}

uint64_t Engine::save_clip(int pre, int post)
{
	uint64_t id = next_clip_id_++;
	if (!ring_ || !ring_->active()) {
		ClipResult r;
		r.id = id;
		r.error = "The buffer is paused";
		if (events_.clip_done)
			events_.clip_done(r);
		return id;
	}

	ClipRequest req;
	req.id = id;
	req.press_sys_usec = (int64_t)(os_gettime_ns() / 1000);
	req.pre_seconds = pre;
	req.post_seconds = post;
	req.format = container_format();
	req.path = make_path("", container_ext());
	req.game = game();
	ring_->request_clip(req);
	HL_INFO("Clip requested: %d s + %d s", pre, post);
	return id;
}

void Engine::bookmark()
{
	if (ring_)
		ring_->add_bookmark((int64_t)(os_gettime_ns() / 1000));
}

void Engine::screenshot()
{
	/* make_path() reads the game name under game_mutex_: build the path before
	 * taking the lock (locking it twice on one thread aborts the process). */
	std::string path = make_path("_shot", "png");
	/* Game capture with no game hooked has a black canvas: take the screen instead. */
	if (settings_.clipping.source == "game" && !game_hooked_ && !fallback_on_) {
		save_screen_grab(path);
		return;
	}
	{
		std::lock_guard lock(game_mutex_);
		pending_screenshot_path_ = path;
	}
	screenshot_requested_ = true;
}

/* Plain GDI copy of the monitor under the mouse: works for the desktop and
 * windowed apps, which is all a black canvas can mean. */
static bool grab_screen(std::vector<uint8_t> &px, uint32_t &w, uint32_t &h)
{
	POINT pt = {};
	GetCursorPos(&pt);
	MONITORINFO mi = {sizeof(mi)};
	if (!GetMonitorInfoW(MonitorFromPoint(pt, MONITOR_DEFAULTTOPRIMARY), &mi))
		return false;
	const int x = mi.rcMonitor.left, y = mi.rcMonitor.top;
	w = (uint32_t)(mi.rcMonitor.right - x);
	h = (uint32_t)(mi.rcMonitor.bottom - y);
	if (!w || !h)
		return false;
	HDC screen = GetDC(nullptr);
	HDC mem = CreateCompatibleDC(screen);
	BITMAPINFO bi = {};
	bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
	bi.bmiHeader.biWidth = (LONG)w;
	bi.bmiHeader.biHeight = -(LONG)h;
	bi.bmiHeader.biPlanes = 1;
	bi.bmiHeader.biBitCount = 32;
	bi.bmiHeader.biCompression = BI_RGB;
	void *bits = nullptr;
	HBITMAP dib = CreateDIBSection(screen, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
	bool ok = false;
	if (dib && bits) {
		HGDIOBJ old = SelectObject(mem, dib);
		ok = BitBlt(mem, 0, 0, (int)w, (int)h, screen, x, y, SRCCOPY | CAPTUREBLT) != 0;
		if (ok) {
			px.assign((const uint8_t *)bits, (const uint8_t *)bits + (size_t)w * h * 4);
			for (size_t i = 3; i < px.size(); i += 4)
				px[i] = 255;
		}
		SelectObject(mem, old);
	}
	if (dib)
		DeleteObject(dib);
	DeleteDC(mem);
	ReleaseDC(nullptr, screen);
	return ok;
}

/* True when a frame has no picture at all (sampled). */
static bool is_black(const std::vector<uint8_t> &px)
{
	const size_t step = std::max<size_t>(4, (px.size() / 4 / 4096) * 4);
	for (size_t i = 0; i + 2 < px.size(); i += step)
		if (px[i] > 6 || px[i + 1] > 6 || px[i + 2] > 6)
			return false;
	return true;
}

void Engine::save_screen_grab(const std::string &path)
{
	if (shot_thread_.joinable())
		shot_thread_.join();
	shot_thread_ = std::thread([this, path]() {
		std::vector<uint8_t> px;
		uint32_t w = 0, h = 0;
		bool ok = grab_screen(px, w, h) && write_image(widen(path), px.data(), (int)w, (int)h, (int)w * 4, "png");
		HL_INFO("Screenshot (screen) %s: %s", ok ? "saved" : "failed", path.c_str());
		if (events_.screenshot_done)
			events_.screenshot_done(ok, path);
	});
}

void Engine::on_rendered()
{
	if (stage_) {
		if (++stage_frames_ < 2)
			return; /* give the GPU a frame to finish the copy */
		gs_stagesurf_t *stage = (gs_stagesurf_t *)stage_;
		uint8_t *data = nullptr;
		uint32_t linesize = 0;
		if (gs_stagesurface_map(stage, &data, &linesize)) {
			const uint32_t w = stage_w_, h = stage_h_;
			std::vector<uint8_t> px((size_t)w * h * 4);
			for (uint32_t y = 0; y < h; y++)
				memcpy(px.data() + (size_t)y * w * 4, data + (size_t)y * linesize, (size_t)w * 4);
			gs_stagesurface_unmap(stage);
			if (stage_rgba_) {
				for (size_t i = 0; i < px.size(); i += 4)
					std::swap(px[i], px[i + 2]);
			}
			for (size_t i = 3; i < px.size(); i += 4)
				px[i] = 255;

			std::string path;
			{
				std::lock_guard lock(game_mutex_);
				path = pending_screenshot_path_;
			}
			if (shot_thread_.joinable())
				shot_thread_.join();
			const bool black = is_black(px);
			shot_thread_ = std::thread([this, px = std::move(px), w = w, h = h, path, black]() mutable {
				if (black) {
					/* nothing on the canvas (capture not showing anything): take the screen */
					uint32_t sw = 0, sh = 0;
					std::vector<uint8_t> screen;
					if (grab_screen(screen, sw, sh)) {
						px = std::move(screen);
						w = sw;
						h = sh;
					}
				}
				bool ok = write_image(widen(path), px.data(), (int)w, (int)h, (int)w * 4, "png");
				HL_INFO("Screenshot %s: %s", ok ? "saved" : "failed", path.c_str());
				if (events_.screenshot_done)
					events_.screenshot_done(ok, path);
			});
		}
		gs_stagesurface_destroy(stage);
		stage_ = nullptr;
		return;
	}

	if (!screenshot_requested_.exchange(false))
		return;
	gs_texture_t *tex = obs_get_main_texture();
	if (!tex) {
		HL_WARN("Screenshot: no frame to capture yet");
		if (events_.screenshot_done)
			events_.screenshot_done(false, "");
		return;
	}
	gs_color_format fmt = gs_texture_get_color_format(tex);
	if (fmt != GS_RGBA && fmt != GS_BGRA && fmt != GS_BGRX) {
		HL_WARN("Screenshot: unsupported canvas format %d", (int)fmt);
		if (events_.screenshot_done)
			events_.screenshot_done(false, "");
		return;
	}
	stage_w_ = gs_texture_get_width(tex);
	stage_h_ = gs_texture_get_height(tex);
	stage_rgba_ = fmt == GS_RGBA;
	gs_stagesurf_t *stage = gs_stagesurface_create(stage_w_, stage_h_, fmt);
	if (!stage) {
		HL_WARN("Screenshot: could not create a %ux%u staging surface", stage_w_, stage_h_);
		if (events_.screenshot_done)
			events_.screenshot_done(false, "");
		return;
	}
	gs_stage_texture(stage, tex);
	stage_ = stage;
	stage_frames_ = 0;
}

bool Engine::toggle_recording(std::string *err)
{
	if (recording_active_) {
		if (record_)
			obs_output_stop(record_);
		return true;
	}

	record_path_ = make_path("_session", "mkv");
	obs_data_t *s = obs_data_create();
	obs_data_set_string(s, "path", record_path_.c_str());
	obs_data_set_string(s, "muxer_settings", "");
	if (!record_) {
		record_ = obs_output_create("ffmpeg_muxer", "Highlight Recording", s, nullptr);
		if (record_)
			signal_handler_connect(obs_output_get_signal_handler(record_), "stop", record_stop_cb, this);
	} else {
		obs_output_update(record_, s);
	}
	obs_data_release(s);
	if (!record_) {
		if (err)
			*err = "Could not create the recording output";
		return false;
	}

	obs_output_set_video_encoder(record_, venc_);
	for (size_t i = 0; i < aencs_.size(); i++)
		obs_output_set_audio_encoder(record_, aencs_[i], i);

	if (!obs_output_start(record_)) {
		const char *last = obs_output_get_last_error(record_);
		if (err)
			*err = last && *last ? last : "The recording could not start";
		return false;
	}
	recording_active_ = true;
	record_started_ns_ = (int64_t)os_gettime_ns();
	HL_INFO("Recording started: %s", record_path_.c_str());
	if (events_.recording_changed)
		events_.recording_changed(true, record_path_, "");
	return true;
}

int64_t Engine::recording_seconds() const
{
	if (!recording_active_)
		return 0;
	return ((int64_t)os_gettime_ns() - record_started_ns_) / 1000000000;
}

void Engine::on_recording_stopped(int code)
{
	recording_active_ = false;
	std::string path = record_path_;
	std::string error;
	if (code != 0 /* OBS_OUTPUT_SUCCESS */)
		error = "Recording stopped unexpectedly (code " + std::to_string(code) + ")";
	HL_INFO("Recording stopped (%d)", code);

	const bool to_mp4 = settings_.video.container != "mkv";
	if (error.empty() && to_mp4) {
		std::string mp4 = path.substr(0, path.size() - 4) + "." + container_ext();
		std::string fmt = container_format();
		std::thread([this, path, mp4, fmt]() {
			std::string e;
			bool ok = media::remux(path, mp4, fmt, &e);
			if (ok)
				DeleteFileW(widen(path).c_str());
			else
				HL_WARN("Remux failed, keeping MKV: %s", e.c_str());
			if (events_.recording_changed)
				events_.recording_changed(false, ok ? mp4 : path, "");
		}).detach();
		return;
	}
	if (events_.recording_changed)
		events_.recording_changed(false, path, error);
}

} // namespace hl

#include "app_ui.h"

#include "../common/image.h"
#include "../common/strings.h"
#include "../media/remux.h"
#include "../media/thumbnail.h"
#include "../media/waveform.h"

#include <imgui_internal.h>

#include <algorithm>
#include <cmath>
#include <ctime>
#include <filesystem>

namespace ui {

namespace fs = std::filesystem;

static std::string fmt_time(double t, bool fraction = true)
{
	if (t < 0)
		t = 0;
	int m = (int)(t / 60);
	double s = t - m * 60;
	char buf[32];
	if (fraction)
		snprintf(buf, sizeof(buf), "%d:%05.2f", m, s);
	else
		snprintf(buf, sizeof(buf), "%d:%02d", m, (int)s);
	return buf;
}

static std::string unique_path(const fs::path &dir, const std::string &stem, const std::string &ext)
{
	fs::path p = dir / hl::widen(stem + ext);
	std::error_code ec;
	for (int i = 2; fs::exists(p, ec) && i < 1000; i++)
		p = dir / hl::widen(stem + " (" + std::to_string(i) + ")" + ext);
	return hl::narrow(p.wstring());
}

/* ------------------------------------------------------------------ */

void AppUI::open_editor(const ClipItem &clip)
{
	editor_close();
	auto ed = std::make_unique<Editor>();
	ed->clip = clip;
	ed->info = hl::media::probe(clip.path);
	if (!ed->info.ok) {
		notice("Could not open the clip: " + ed->info.error, true);
		return;
	}

	for (const auto &a : ed->info.audio) {
		if (a.title == "Mix")
			ed->mix_stream = a.index;
		else {
			ed->lanes.push_back(a.index);
			ed->lane_names.push_back(a.title);
		}
	}
	if (ed->lanes.empty() && ed->mix_stream >= 0) {
		ed->lanes.push_back(ed->mix_stream);
		ed->lane_names.push_back("Audio");
		ed->mix_stream = -1;
	}
	ed->lane_on.assign(ed->lanes.size(), true);
	ed->peaks.resize(ed->lanes.size());
	ed->in = 0;
	ed->out = ed->info.duration;
	ed->strip.resize(12);
	ed->strip_px.resize(12);
	ed->strip_wh.assign(24, 0);

	std::string err;
	if (!ed->player.open(clip.path, 1280, 720, &err)) {
		notice("Preview unavailable: " + err, true);
	}

	Editor *e = ed.get();
	ed_ = std::move(ed);
	e->player.set_audio_streams(editor_selected_streams(false));
	e->audio.start([e](float *dst, size_t frames) { return e->player.read_audio(dst, frames); });

	/* waveforms and filmstrip in the background */
	e->loader = std::thread([e]() {
		const double dur = e->info.duration;
		for (int i = 0; i < 12 && !e->loader_stop; i++) {
			std::vector<uint8_t> px;
			int w = 0, h = 0;
			double t = dur * (i + 0.5) / 12.0;
			if (hl::media::decode_thumbnail(e->clip.path, t, 192, 108, px, w, h)) {
				std::lock_guard lock(e->extra_mutex);
				e->strip_px[i] = std::move(px);
				e->strip_wh[i * 2] = w;
				e->strip_wh[i * 2 + 1] = h;
			}
		}
		for (size_t i = 0; i < e->lanes.size() && !e->loader_stop; i++) {
			std::vector<float> peaks;
			hl::media::audio_peaks(e->clip.path, e->lanes[i], 480, peaks);
			std::lock_guard lock(e->extra_mutex);
			e->peaks[i] = std::move(peaks);
		}
	});

	e->open = true;
	page_ = Page::Editor;
}

void AppUI::editor_close()
{
	if (!ed_)
		return;
	ed_->loader_stop = true;
	if (ed_->loader.joinable())
		ed_->loader.join();
	ed_->audio.stop();
	ed_->player.close();
	texture_release(ed_->frame);
	for (auto &t : ed_->strip)
		texture_release(t);
	ed_.reset();
}

std::vector<int> AppUI::editor_selected_streams(bool for_lossless) const
{
	std::vector<int> out;
	if (!ed_)
		return out;
	bool all = std::all_of(ed_->lane_on.begin(), ed_->lane_on.end(), [](bool b) { return b; });
	if (all && ed_->mix_stream >= 0) {
		if (for_lossless) {
			for (const auto &a : ed_->info.audio)
				out.push_back(a.index);
		} else {
			out.push_back(ed_->mix_stream);
		}
		return out;
	}
	for (size_t i = 0; i < ed_->lanes.size(); i++)
		if (ed_->lane_on[i])
			out.push_back(ed_->lanes[i]);
	return out;
}

/* ------------------------------------------------------------------ */

void AppUI::editor_keyboard()
{
	if (!ed_ || ImGui::GetIO().WantTextInput || share_open_)
		return;
	auto &pl = ed_->player;
	const double step = ed_->info.fps > 0 ? 1.0 / ed_->info.fps : 1.0 / 60;
	const bool shift = ImGui::GetIO().KeyShift;
	if (ImGui::IsKeyPressed(ImGuiKey_Space, false)) {
		if (pl.playing())
			pl.pause();
		else {
			if (pl.position() >= ed_->out - 0.05 || pl.position() < ed_->in - 0.05)
				pl.seek(ed_->in);
			pl.play();
		}
	}
	if (ImGui::IsKeyPressed(ImGuiKey_LeftArrow)) {
		pl.pause();
		pl.seek(pl.position() - (shift ? 5.0 : step));
	}
	if (ImGui::IsKeyPressed(ImGuiKey_RightArrow)) {
		pl.pause();
		pl.seek(pl.position() + (shift ? 5.0 : step));
	}
	if (ImGui::IsKeyPressed(ImGuiKey_I, false))
		ed_->in = std::min(pl.position(), ed_->out - 0.5);
	if (ImGui::IsKeyPressed(ImGuiKey_O, false))
		ed_->out = std::max(pl.position(), ed_->in + 0.5);
	if (ImGui::IsKeyPressed(ImGuiKey_Home, false))
		pl.seek(ed_->in);
	if (ImGui::IsKeyPressed(ImGuiKey_End, false))
		pl.seek(ed_->out);
}

void AppUI::editor_page()
{
	Editor &e = *ed_;
	auto &pl = e.player;
	editor_keyboard();

	/* stop at the out point */
	if (pl.playing() && pl.position() >= e.out) {
		pl.pause();
		pl.seek(e.out);
	}

	/* upload filmstrip frames as they arrive */
	{
		std::lock_guard lock(e.extra_mutex);
		for (int i = 0; i < 12; i++)
			if (!e.strip_px[i].empty() && !e.strip[i]) {
				texture_create(e.strip[i], e.strip_px[i].data(), e.strip_wh[i * 2], e.strip_wh[i * 2 + 1]);
				e.strip_px[i].clear();
			}
	}
	int fw = 0, fh = 0;
	if (pl.take_frame(e.px, fw, fh))
		texture_update(e.frame, e.px.data(), fw, fh);

	/* toolbar */
	ImDrawList *dl = ImGui::GetWindowDrawList();
	const float avail = ImGui::GetContentRegionAvail().x;
	const float row_x = ImGui::GetCursorPosX();
	if (secondary_button("Clips##back", ImVec2(px(96), px(44)), Icon::ChevronLeft)) {
		editor_close();
		page_ = Page::Library;
		return;
	}
	ImGui::SameLine(0, px(16));
	ImGui::BeginGroup();
	{
		ImVec2 p = ImGui::GetCursorScreenPos();
		text_ellipsis(dl, fonts.title, p, avail - px(330), col::text, e.clip.name);
		time_t tt = (time_t)e.clip.mtime;
		tm lt;
		localtime_s(&lt, &tt);
		char when[32];
		strftime(when, sizeof(when), "%b %d %H:%M", &lt);
		std::string meta = e.clip.game + " · " + when + " · " + hl::format_duration(e.info.duration) + " · " +
				   std::to_string(e.info.height) + "p" + std::to_string((int)(e.info.fps + 0.5)) + " · " +
				   hl::to_lower(e.info.video_codec) + " · " + hl::format_bytes(e.info.file_size);
		text_ellipsis(dl, fonts.mono_small, ImVec2(p.x, p.y + px(26)), avail - px(330), col::muted, meta);
		ImGui::Dummy(ImVec2(avail - px(330), px(44)));
	}
	ImGui::EndGroup();
	ImGui::SameLine(row_x + avail - px(44) * 3 - px(16));
	bool fav = library_.is_favorite(e.clip.path);
	if (icon_button("##fav", fav ? Icon::StarFilled : Icon::Star, px(44), fav ? "Remove from favourites" : "Favourite",
			fav ? col::accent : col::muted, true)) {
		library_.set_favorite(e.clip.path, !fav);
		library_.save();
	}
	ImGui::SameLine(0, px(8));
	if (icon_button("##folder", Icon::Folder, px(44), "Show in folder", col::muted, true))
		show_in_folder(e.clip.path);
	ImGui::SameLine(0, px(8));
	if (icon_button("##del", Icon::Trash, px(44), "Delete clip", col::muted, true))
		ImGui::OpenPopup("Delete clip?");
	ImGui::SetNextWindowSize(ImVec2(px(420), 0));
	ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(px(20), px(20)));
	if (ImGui::BeginPopupModal("Delete clip?", nullptr, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize)) {
		text(fonts.h2, col::text, "Delete this clip?");
		text_wrapped(fonts.body, col::muted, px(380), "It goes to the Recycle Bin, so you can still get it back.");
		if (secondary_button("Keep it##del", ImVec2(px(140), px(44))))
			ImGui::CloseCurrentPopup();
		ImGui::SameLine();
		if (primary_button("DELETE##del", ImVec2(-1, px(44)))) {
			std::string path = e.clip.path;
			ImGui::CloseCurrentPopup();
			ImGui::EndPopup();
			ImGui::PopStyleVar();
			editor_close();
			delete_clip(path);
			page_ = Page::Library;
			return;
		}
		ImGui::EndPopup();
	}
	ImGui::PopStyleVar();

	/* layout */
	const bool side = avail > px(1060);
	const float panel_w = side ? px(330) : avail;
	const float main_w = side ? avail - panel_w - px(20) : avail;

	ImGui::BeginGroup();
	{
		/* player: leave room for the transport row and the whole timeline */
		float ph = main_w * 9.0f / 16.0f;
		const float timeline_h = px(196) + (float)e.lanes.size() * px(52);
		const float room = ImGui::GetContentRegionAvail().y - px(62) - timeline_h - px(28);
		const float max_h = std::max(px(200), room);
		float pw = main_w;
		if (ph > max_h)
			ph = max_h;
		ImVec2 p = ImGui::GetCursorScreenPos();
		bool clicked = ImGui::InvisibleButton("##video", ImVec2(pw, ph));
		dl->AddRectFilled(p, ImVec2(p.x + pw, p.y + ph), IM_COL32(0x06, 0x07, 0x08, 255), px(12));
		if (e.frame) {
			float ar = (float)e.frame.w / e.frame.h;
			float w = pw, h = pw / ar;
			if (h > ph) {
				h = ph;
				w = ph * ar;
			}
			ImVec2 a(p.x + (pw - w) / 2, p.y + (ph - h) / 2);
			dl->AddImageRounded(e.frame.id(), a, ImVec2(a.x + w, a.y + h), ImVec2(0, 0), ImVec2(1, 1),
					    IM_COL32_WHITE, px(10));
		}
		std::string badge = "PREVIEW · " + std::to_string(e.info.height) + "p" + std::to_string((int)(e.info.fps + 0.5));
		ImVec2 bs = measure(fonts.mono_small, badge.c_str());
		dl->AddRectFilled(ImVec2(p.x + px(14), p.y + px(14)), ImVec2(p.x + px(28) + bs.x, p.y + px(22) + bs.y),
				  IM_COL32(0x0B, 0x0C, 0x0E, 210), px(6));
		dl->AddText(fonts.mono_small, fonts.mono_small->FontSize, ImVec2(p.x + px(21), p.y + px(18)), col::text,
			    badge.c_str());
		if (ImGui::IsItemHovered() && !pl.playing()) {
			ImVec2 cc(p.x + pw / 2, p.y + ph / 2);
			dl->AddCircleFilled(cc, px(30), IM_COL32(0x0B, 0x0C, 0x0E, 170));
			draw_icon(dl, Icon::Play, ImVec2(cc.x + px(2), cc.y), px(22), col::text);
		}
		if (clicked) {
			if (pl.playing())
				pl.pause();
			else {
				if (pl.position() >= e.out - 0.05 || pl.position() < e.in - 0.05)
					pl.seek(e.in);
				pl.play();
			}
		}

		/* transport */
		const double step = e.info.fps > 0 ? 1.0 / e.info.fps : 1.0 / 60;
		if (icon_button("##prev", Icon::FrameBack, px(44), "Previous frame (Left)")) {
			pl.pause();
			pl.seek(pl.position() - step);
		}
		ImGui::SameLine(0, px(4));
		{
			ImVec2 bp = ImGui::GetCursorScreenPos();
			bool c2 = ImGui::InvisibleButton("##play", ImVec2(px(48), px(48)));
			bool hov = ImGui::IsItemHovered();
			dl->AddCircleFilled(ImVec2(bp.x + px(24), bp.y + px(24)), px(24), hov ? IM_COL32_WHITE : col::text);
			draw_icon(dl, pl.playing() ? Icon::Pause : Icon::Play,
				  ImVec2(bp.x + px(pl.playing() ? 24 : 25.5f), bp.y + px(24)), px(18), col::ink);
			if (c2) {
				if (pl.playing())
					pl.pause();
				else {
					if (pl.position() >= e.out - 0.05 || pl.position() < e.in - 0.05)
						pl.seek(e.in);
					pl.play();
				}
			}
		}
		ImGui::SameLine(0, px(4));
		if (icon_button("##next", Icon::FrameForward, px(44), "Next frame (Right)")) {
			pl.pause();
			pl.seek(pl.position() + step);
		}
		ImGui::SameLine(0, px(12));
		{
			std::string t = fmt_time(pl.position());
			std::string d = " / " + fmt_time(e.info.duration);
			ImVec2 cp = ImGui::GetCursorScreenPos();
			dl->AddText(fonts.mono, fonts.mono->FontSize, ImVec2(cp.x, cp.y + px(15)), col::text, t.c_str());
			float tw = measure(fonts.mono, t.c_str()).x;
			dl->AddText(fonts.mono, fonts.mono->FontSize, ImVec2(cp.x + tw, cp.y + px(15)), col::dim, d.c_str());
			ImGui::Dummy(ImVec2(tw + measure(fonts.mono, d.c_str()).x, px(48)));
		}
		ImGui::SameLine(main_w - px(44));
		if (icon_button("##frame", Icon::Camera, px(44), "Save this frame as a PNG")) {
			std::string path = e.clip.path;
			double t = pl.position();
			fs::path src = hl::widen(path);
			char suffix[32];
			snprintf(suffix, sizeof(suffix), "_frame_%d-%02d", (int)(t / 60), (int)t % 60);
			std::string out = unique_path(src.parent_path(), hl::narrow(src.stem().wstring()) + suffix, ".png");
			std::thread([this, path, t, out]() {
				std::vector<uint8_t> px2;
				int w = 0, h = 0;
				bool ok = hl::media::decode_thumbnail(path, t, 7680, 4320, px2, w, h) &&
					  hl::write_image(hl::widen(out), px2.data(), w, h, w * 4, "png");
				if (ok)
					rescan_ = true;
			}).detach();
			notice("Saving frame…");
		}

		editor_timeline(main_w);
	}
	ImGui::EndGroup();

	if (side)
		ImGui::SameLine(0, px(20));
	editor_export_panel(panel_w);
}

/* ------------------------------------------------------------------ */

void AppUI::editor_timeline(float width)
{
	Editor &e = *ed_;
	auto &pl = e.player;
	const double dur = std::max(0.1, e.info.duration);

	begin_card("##timeline", width);
	ImDrawList *dl = ImGui::GetWindowDrawList();
	const float inner = ImGui::GetContentRegionAvail().x;

	/* header */
	text(fonts.h2, col::text, "Trim");
	ImGui::SameLine(0, px(12));
	auto chip = [&](const char *label, const std::string &value, bool accent) {
		ImVec2 p = ImGui::GetCursorScreenPos();
		ImVec2 ls = measure(fonts.mono_small, label);
		ImVec2 vs = measure(fonts.mono_small, value.c_str());
		float w = ls.x + vs.x + px(22);
		dl->AddRectFilled(ImVec2(p.x, p.y + px(1)), ImVec2(p.x + w, p.y + px(25)), col::bg3, px(6));
		dl->AddText(fonts.mono_small, fonts.mono_small->FontSize, ImVec2(p.x + px(8), p.y + px(6)), col::dim, label);
		dl->AddText(fonts.mono_small, fonts.mono_small->FontSize, ImVec2(p.x + px(14) + ls.x, p.y + px(6)),
			    accent ? col::accent_text : col::text, value.c_str());
		ImGui::Dummy(ImVec2(w, px(26)));
	};
	chip("IN", fmt_time(e.in), false);
	ImGui::SameLine(0, px(6));
	chip("OUT", fmt_time(e.out), false);
	ImGui::SameLine(0, px(6));
	chip("LENGTH", fmt_time(e.out - e.in), true);
	if (inner > px(620)) {
		ImGui::SameLine(inner - px(250));
		if (secondary_button("Set in  I##in", ImVec2(px(80), px(30))))
			e.in = std::min(pl.position(), e.out - 0.5);
		ImGui::SameLine(0, px(6));
		if (secondary_button("Set out  O##out", ImVec2(px(90), px(30))))
			e.out = std::max(pl.position(), e.in + 0.5);
		ImGui::SameLine(0, px(6));
		if (secondary_button("Reset##trim", ImVec2(px(62), px(30)))) {
			e.in = 0;
			e.out = e.info.duration;
		}
	}

	/* geometry */
	const float label_w = px(156);
	const float gap = px(12);
	ImVec2 origin = ImGui::GetCursorScreenPos();
	const float track_x = origin.x + label_w + gap;
	const float track_w = inner - label_w - gap;
	auto x_of = [&](double t) { return track_x + (float)(t / dur) * track_w; };
	auto t_of = [&](float x) { return std::clamp((double)(x - track_x) / track_w * dur, 0.0, dur); };

	/* ruler */
	const float ruler_h = px(20);
	double step = dur <= 30 ? 5 : dur <= 120 ? 10 : dur <= 600 ? 30 : dur <= 1800 ? 120 : 600;
	for (double t = 0; t <= dur + 0.001; t += step) {
		float x = x_of(t);
		std::string lbl = hl::format_duration(t);
		float lw = measure(fonts.mono_small, lbl.c_str()).x;
		float lx = t == 0 ? x : std::min(x - lw / 2, track_x + track_w - lw);
		dl->AddText(fonts.mono_small, fonts.mono_small->FontSize, ImVec2(lx, origin.y + px(3)), col::dim, lbl.c_str());
	}
	for (const auto &m : e.info.chapters) {
		if (m.title.rfind("Bookmark", 0) != 0)
			continue;
		float x = x_of(m.time);
		draw_icon(dl, Icon::Diamond, ImVec2(x, origin.y + px(10)), px(11), col::accent);
		ImGui::SetCursorScreenPos(ImVec2(x - px(7), origin.y + px(3)));
		ImGui::PushID(&m);
		if (ImGui::InvisibleButton("##bm", ImVec2(px(14), px(14)))) {
			pl.pause();
			pl.seek(m.time);
		}
		if (ImGui::IsItemHovered())
			ImGui::SetTooltip("%s at %s", m.title.c_str(), fmt_time(m.time).c_str());
		ImGui::PopID();
	}

	/* lanes */
	const float lanes_y = origin.y + ruler_h + px(6);
	const float video_h = px(52), lane_h = px(44), lane_gap = px(8);
	float y = lanes_y;

	/* video lane */
	{
		dl->AddText(fonts.mono_small, fonts.mono_small->FontSize, ImVec2(origin.x, y + px(18)), col::muted, "V");
		dl->AddText(fonts.body_medium, fonts.body_medium->FontSize, ImVec2(origin.x + px(22), y + px(16)), col::text,
			    "Video");
		float cw = track_w / 12.0f;
		for (int i = 0; i < 12; i++) {
			ImVec2 a(track_x + i * cw, y), b(track_x + (i + 1) * cw - px(2), y + video_h);
			if (e.strip[i])
				dl->AddImage(e.strip[i].id(), a, b);
			else
				dl->AddRectFilled(a, b, IM_COL32(0x2A, 0x1C, 0x22, 255));
		}
		y += video_h + lane_gap;
	}

	std::vector<std::vector<float>> peaks;
	{
		std::lock_guard lock(e.extra_mutex);
		peaks = e.peaks;
	}
	bool streams_changed = false;
	for (size_t i = 0; i < e.lanes.size(); i++) {
		ImGui::PushID((int)i);
		std::string tag = "T" + std::to_string(i + (e.mix_stream >= 0 ? 2 : 1));
		ImVec2 ts = measure(fonts.mono_small, tag.c_str());
		dl->AddRectFilled(ImVec2(origin.x, y + px(13)), ImVec2(origin.x + ts.x + px(10), y + px(31)), col::bg3, px(4));
		dl->AddText(fonts.mono_small, fonts.mono_small->FontSize, ImVec2(origin.x + px(5), y + px(15)), col::muted,
			    tag.c_str());
		text_ellipsis(dl, fonts.body_medium, ImVec2(origin.x + ts.x + px(18), y + px(13)), label_w - ts.x - px(62),
			      col::text, e.lane_names[i]);
		ImGui::SetCursorScreenPos(ImVec2(origin.x + label_w - px(42), y + px(2)));
		bool on = e.lane_on[i];
		if (icon_button("##mute", on ? Icon::Speaker : Icon::SpeakerMute, px(40), on ? "Mute this track" : "Unmute",
				on ? col::text : col::dim)) {
			e.lane_on[i] = !on;
			streams_changed = true;
		}

		ImVec2 a(track_x, y), b(track_x + track_w, y + lane_h);
		dl->AddRectFilled(a, b, col::bg0, px(6));
		if (i < peaks.size() && !peaks[i].empty()) {
			const auto &pk = peaks[i];
			const float bar = px(3), bgap = px(2);
			int bars = std::max(1, (int)(track_w / (bar + bgap)));
			for (int k = 0; k < bars; k++) {
				float t0 = (float)k / bars, t1 = (float)(k + 1) / bars;
				size_t s0 = (size_t)(t0 * pk.size()), s1 = std::max(s0 + 1, (size_t)(t1 * pk.size()));
				float v = 0;
				for (size_t s = s0; s < s1 && s < pk.size(); s++)
					v = std::max(v, pk[s]);
				float h = std::max(px(2), v * (lane_h - px(8)));
				float x = track_x + k * (bar + bgap) + px(2);
				double tt = (t0 + t1) / 2 * dur;
				ImU32 c = !on ? col::off : (tt >= e.in && tt <= e.out) ? col::wave : col::wave_dim;
				dl->AddRectFilled(ImVec2(x, y + (lane_h - h) / 2), ImVec2(x + bar, y + (lane_h + h) / 2), c, px(1));
			}
		}
		y += lane_h + lane_gap;
		ImGui::PopID();
	}
	const float lanes_bottom = y - lane_gap;
	if (streams_changed)
		pl.set_audio_streams(editor_selected_streams(false));

	/* scrub area (under the handles) */
	ImGui::SetCursorScreenPos(ImVec2(track_x, lanes_y));
	ImGui::SetNextItemAllowOverlap();
	ImGui::InvisibleButton("##scrub", ImVec2(track_w, lanes_bottom - lanes_y));
	if (ImGui::IsItemActive()) {
		pl.pause();
		pl.seek(t_of(ImGui::GetIO().MousePos.x));
	}

	/* trim overlay */
	float xi = x_of(e.in), xo = x_of(e.out);
	dl->AddRectFilled(ImVec2(track_x, lanes_y), ImVec2(xi, lanes_bottom), col::shade);
	dl->AddRectFilled(ImVec2(xo, lanes_y), ImVec2(track_x + track_w, lanes_bottom), col::shade);
	dl->AddLine(ImVec2(xi, lanes_y), ImVec2(xo, lanes_y), col::accent, px(2));
	dl->AddLine(ImVec2(xi, lanes_bottom), ImVec2(xo, lanes_bottom), col::accent, px(2));

	auto handle = [&](const char *id, double &value, bool is_in) {
		float x = x_of(value);
		ImVec2 a(x - px(7), lanes_y - px(4)), b(x + px(7), lanes_bottom + px(4));
		ImGui::SetCursorScreenPos(a);
		ImGui::InvisibleButton(id, ImVec2(b.x - a.x, b.y - a.y));
		bool hov = ImGui::IsItemHovered() || ImGui::IsItemActive();
		if (hov)
			ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
		if (ImGui::IsItemActive()) {
			double t = t_of(ImGui::GetIO().MousePos.x);
			value = is_in ? std::min(t, e.out - 0.5) : std::max(t, e.in + 0.5);
			value = std::clamp(value, 0.0, dur);
			pl.pause();
			pl.seek(value);
		}
		dl->AddRectFilled(a, b, hov ? col::accent_text : col::accent, px(4));
		dl->AddLine(ImVec2(x, (a.y + b.y) / 2 - px(9)), ImVec2(x, (a.y + b.y) / 2 + px(9)), col::ink, px(2));
	};
	handle("##in", e.in, true);
	handle("##out", e.out, false);

	/* playhead */
	float xp = x_of(pl.position());
	dl->AddLine(ImVec2(xp, lanes_y - px(10)), ImVec2(xp, lanes_bottom), col::text, px(2));
	dl->AddRectFilled(ImVec2(xp - px(6), lanes_y - px(12)), ImVec2(xp + px(6), lanes_y - px(1)), col::text, px(3));

	ImGui::SetCursorScreenPos(ImVec2(origin.x, lanes_bottom + px(4)));
	ImGui::Dummy(ImVec2(inner, 0));
	end_card();
}

/* ------------------------------------------------------------------ */

void AppUI::start_trim_job(bool replace)
{
	if (trim_job_.running || !ed_)
		return;
	trim_job_.join();
	trim_job_.done = false;
	trim_job_.ok = false;
	trim_job_.error.clear();
	trim_job_.result.clear();
	trim_job_.progress = 0;
	trim_job_.cancel = false;

	Editor &e = *ed_;
	const fs::path src = hl::widen(e.clip.path);
	const std::string stem = hl::narrow(src.stem().wstring());
	const bool exact = e.cut_mode == 1;
	const std::string ext = exact ? ".mp4" : (e.format == 1 ? ".mkv" : ".mp4");
	const std::string out = replace ? hl::narrow((src.parent_path() / (src.stem().wstring() + L".trim_tmp" +
									   src.extension().wstring()))
							     .wstring())
					: unique_path(src.parent_path(), stem + "_trim", ext);
	const std::string in_path = e.clip.path;
	const double t_in = e.in, t_out = e.out >= e.info.duration - 0.01 ? -1 : e.out;
	const std::vector<int> streams = editor_selected_streams(!exact);
	const std::string format = replace ? (src.extension() == L".mkv" ? "matroska" : "mp4") : (e.format == 1 ? "matroska" : "mp4");

	std::string codec = hl::to_lower(e.info.video_codec);
	if (codec != "hevc" && codec != "av1")
		codec = "h264";
	hl::media::ShareRequest req;
	req.input = in_path;
	req.output = out;
	req.start = t_in;
	req.end = t_out;
	req.audio_streams = streams;
	req.codec = codec;
	req.target_bytes = 0;
	hl::media::SharePlan plan = hl::media::plan_share(e.info, req);

	if (replace) {
		/* the preview holds the file open */
		editor_close();
		page_ = Page::Library;
	}

	trim_job_.running = true;
	trim_job_.thread = std::thread([this, exact, replace, in_path, out, t_in, t_out, streams, format, req, plan]() {
		std::string err;
		bool ok;
		if (exact) {
			ok = hl::media::run_share(
				req, plan,
				[this](float p) {
					trim_job_.progress = p;
					return !trim_job_.cancel.load();
				},
				&err, nullptr);
		} else {
			hl::media::TrimOptions opt;
			opt.start = t_in;
			opt.end = t_out;
			opt.audio_streams = streams;
			opt.format = format;
			ok = hl::media::trim_lossless(in_path, out, opt, &err);
		}
		std::string final_path = out;
		if (ok && replace) {
			if (!MoveFileExW(hl::widen(out).c_str(), hl::widen(in_path).c_str(), MOVEFILE_REPLACE_EXISTING)) {
				ok = false;
				err = "Could not replace the original (is it open in another app?)";
				DeleteFileW(hl::widen(out).c_str());
			} else {
				final_path = in_path;
			}
		}
		std::lock_guard lock(trim_job_.mutex);
		trim_job_.ok = ok;
		trim_job_.error = err;
		trim_job_.result = final_path;
		trim_job_.done = true;
		trim_job_.running = false;
		rescan_ = true;
	});
}

void AppUI::editor_export_panel(float width)
{
	Editor &e = *ed_;
	begin_card("##export", width);
	const float w = ImGui::GetContentRegionAvail().x;
	text(fonts.title, col::text, "Export");

	text(fonts.small_semibold, col::muted, "Save a trimmed copy");
	if (radio_card("cut0", "Lossless · instant", "Cuts on the nearest keyframe (every second). No quality loss.",
		       e.cut_mode == 0))
		e.cut_mode = 0;
	if (radio_card("cut1", "Frame-exact", "Re-encodes on your GPU at visually lossless quality.", e.cut_mode == 1))
		e.cut_mode = 1;

	if (e.cut_mode == 0) {
		text(fonts.small_semibold, col::muted, "Format");
		pill_grid("fmt", {"MP4", "MKV"}, e.format, 2, true, 40);
	}

	std::string audio;
	for (size_t i = 0; i < e.lanes.size(); i++)
		if (e.lane_on[i])
			audio += (audio.empty() ? "" : ", ") + e.lane_names[i];
	text_wrapped(fonts.caption, col::muted, w, "Audio: %s", audio.empty() ? "none" : audio.c_str());

	double len = e.out - e.in;
	double est = e.info.duration > 0 ? (double)e.info.file_size * len / e.info.duration : 0;
	std::string summary = fmt_time(len) + " · ≈ " + hl::format_bytes((uint64_t)est);
	value_box(summary.c_str(), w);

	bool busy = trim_job_.running;
	if (primary_button("SAVE TRIMMED COPY##trim", ImVec2(w, px(48)), nullptr, !busy))
		start_trim_job(false);
	if (e.cut_mode == 0) {
		if (secondary_button("Replace original##rep", ImVec2(w, px(44)), Icon::Logo, !busy))
			e.confirm_replace = true;
	}

	if (busy) {
		ImVec2 p = ImGui::GetCursorScreenPos();
		ImDrawList *dl = ImGui::GetWindowDrawList();
		dl->AddRectFilled(p, ImVec2(p.x + w, p.y + px(6)), col::bg0, px(3));
		float pr = e.cut_mode == 1 ? trim_job_.progress.load() : 0.5f;
		dl->AddRectFilled(p, ImVec2(p.x + w * pr, p.y + px(6)), col::accent, px(3));
		ImGui::Dummy(ImVec2(w, px(8)));
		text(fonts.caption, col::muted, e.cut_mode == 1 ? "Encoding on your GPU…" : "Copying…");
	} else {
		std::lock_guard lock(trim_job_.mutex);
		if (trim_job_.done) {
			if (trim_job_.ok) {
				std::string name = hl::narrow(fs::path(hl::widen(trim_job_.result)).filename().wstring());
				text_wrapped(fonts.caption, col::text, w, "Saved %s", name.c_str());
				if (secondary_button("Show in folder##trimres", ImVec2(w, px(40)), Icon::Folder))
					show_in_folder(trim_job_.result);
			} else {
				text_wrapped(fonts.caption, col::accent_text, w, "%s", trim_job_.error.c_str());
			}
		}
	}

	ImGui::Dummy(ImVec2(0, px(4)));
	ImGui::Separator();
	text(fonts.small_semibold, col::muted, "Share");
	text_wrapped(fonts.caption, col::muted, w,
		     "Make a smaller copy for Discord and friends. The original stays full size and full quality.");
	if (primary_button("SHARE…##share", ImVec2(w, px(48))))
		open_share();
	end_card();

	if (e.confirm_replace) {
		e.confirm_replace = false;
		ImGui::OpenPopup("Replace original?");
	}
	ImGui::SetNextWindowSize(ImVec2(px(440), 0));
	ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(px(20), px(20)));
	if (ImGui::BeginPopupModal("Replace original?", nullptr, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize)) {
		text(fonts.h2, col::text, "Replace the original clip?");
		text_wrapped(fonts.body, col::muted, px(400),
			     "The parts outside the trim are removed from the file. This cannot be undone.");
		if (secondary_button("Cancel##rep", ImVec2(px(140), px(44))))
			ImGui::CloseCurrentPopup();
		ImGui::SameLine();
		if (primary_button("REPLACE##rep", ImVec2(-1, px(44)))) {
			ImGui::CloseCurrentPopup();
			start_trim_job(true);
		}
		ImGui::EndPopup();
	}
	ImGui::PopStyleVar();
}

} // namespace ui

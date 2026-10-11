#include "app_ui.h"
#include "r2.h"

#include "../common/strings.h"

#include <shellapi.h>

#include <algorithm>
#include <filesystem>

namespace ui {

namespace fs = std::filesystem;

void AppUI::open_share()
{
	if (!ed_ || !ed_->open)
		return;
	share_open_ = true;
	share_popup_pending_ = true;
	if (!share_job_.running) {
		std::lock_guard lock(share_job_.mutex);
		share_job_.done = false;
	}
	ed_->player.pause();
}

static int64_t target_bytes_for(const hl::ShareSettings &s)
{
	const int64_t mb = 1024 * 1024;
	if (s.target == "discord")
		return s.discord_mb * mb;
	if (s.target == "nitro_basic")
		return s.nitro_basic_mb * mb;
	if (s.target == "nitro")
		return s.nitro_mb * mb;
	if (s.target == "custom")
		return s.custom_mb * mb;
	return 0;
}

void AppUI::share_dialog()
{
	if (share_popup_pending_) {
		share_popup_pending_ = false;
		ImGui::OpenPopup("##share");
	}
	if (!share_open_ || !ed_)
		return;

	Editor &e = *ed_;
	auto &sh = settings_.share;
	const ImGuiViewport *vp = ImGui::GetMainViewport();
	const float w = std::min(px(880), vp->Size.x - px(40));
	ImGui::SetNextWindowSize(ImVec2(w, 0));
	ImGui::SetNextWindowPos(ImVec2(vp->Pos.x + vp->Size.x / 2, vp->Pos.y + px(56)), ImGuiCond_Always, ImVec2(0.5f, 0));
	ImGui::PushStyleColor(ImGuiCol_PopupBg, col::bg1);
	ImGui::PushStyleColor(ImGuiCol_Border, IM_COL32(0x2A, 0x2E, 0x36, 255));
	ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(px(24), px(20)));
	ImGui::PushStyleVar(ImGuiStyleVar_PopupRounding, px(18));
	ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(px(10), px(10)));

	bool open = ImGui::BeginPopupModal("##share", nullptr, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
								     ImGuiWindowFlags_NoMove | ImGuiWindowFlags_AlwaysAutoResize);
	if (!open) {
		share_open_ = false;
		ImGui::PopStyleVar(3);
		ImGui::PopStyleColor(2);
		return;
	}

	ImDrawList *dl = ImGui::GetWindowDrawList();
	const float inner = ImGui::GetContentRegionAvail().x;
	const double len = e.out - e.in;
	const double src_est = e.info.duration > 0 ? (double)e.info.file_size * len / e.info.duration : 0;

	/* header */
	{
		ImVec2 p = ImGui::GetCursorScreenPos();
		ImVec2 tq(p.x + px(112), p.y + px(63));
		if (e.frame)
			dl->AddImageRounded(e.frame.id(), p, tq, ImVec2(0, 0), ImVec2(1, 1), IM_COL32_WHITE, px(8));
		else
			dl->AddRectFilled(p, tq, IM_COL32(0x3B, 0x1E, 0x25, 255), px(8));
		std::string title = "Share “" + e.clip.name + "”";
		text_ellipsis(dl, fonts.title, ImVec2(p.x + px(128), p.y + px(6)), inner - px(190), col::text, title);
		char sub[256];
		snprintf(sub, sizeof(sub), "%s → %s · %.1f s · original %dp%d %s · %s", hl::format_duration(e.in).c_str(),
			 hl::format_duration(e.out).c_str(), len, e.info.height, (int)(e.info.fps + 0.5),
			 hl::to_lower(e.info.video_codec).c_str(), hl::format_bytes((uint64_t)src_est).c_str());
		text_ellipsis(dl, fonts.mono_small, ImVec2(p.x + px(128), p.y + px(36)), inner - px(190), col::muted, sub);
		ImGui::SetCursorScreenPos(ImVec2(p.x + inner - px(44), p.y));
		if (icon_button("##close", Icon::Close, px(44), "Close") && !share_job_.running) {
			ImGui::CloseCurrentPopup();
			share_open_ = false;
		}
		ImGui::SetCursorScreenPos(ImVec2(p.x, tq.y + px(10)));
		ImGui::Separator();
	}

	bool done, running = share_job_.running;
	{
		std::lock_guard lock(share_job_.mutex);
		done = share_job_.done;
	}

	hl::media::ShareRequest req;
	req.input = e.clip.path;
	req.start = e.in;
	req.end = e.out >= e.info.duration - 0.01 ? -1 : e.out;
	req.audio_streams = editor_selected_streams(false);
	req.audio_gains = editor_selected_gains();
	req.codec = sh.codec;
	req.target_bytes = target_bytes_for(sh);
	hl::media::SharePlan plan = hl::media::plan_share(e.info, req);

	if (!running && !done) {
		const bool two = inner > px(700);
		const float left_w = two ? inner * 0.54f : inner;
		const float right_w = two ? inner - left_w - px(24) : inner;

		ImGui::BeginGroup();
		ImGui::PushItemWidth(left_w);
		ImGui::BeginChild("##left", ImVec2(left_w, 0), ImGuiChildFlags_AutoResizeY);
		text(fonts.body_bold, col::text, "Where is it going?");
		struct T {
			const char *id, *name, *desc;
			std::string limit;
		} targets[] = {
			{"discord", "Discord", "Free accounts", std::to_string(sh.discord_mb) + " MB"},
			{"nitro_basic", "Discord · Nitro Basic", "Nitro Basic upload limit", std::to_string(sh.nitro_basic_mb) + " MB"},
			{"nitro", "Discord · Nitro", "Nitro upload limit", std::to_string(sh.nitro_mb) + " MB"},
			{"best", "Best quality", "No size limit. Smallest file that still looks the same.", "Auto"},
			{"custom", "Custom size", "Your own limit", std::to_string(sh.custom_mb) + " MB"},
		};
		for (auto &t : targets)
			if (radio_card(t.id, t.name, t.desc, sh.target == t.id, t.limit.c_str())) {
				sh.target = t.id;
				mark_settings_dirty();
			}
		if (sh.target == "custom" && slider_int("##custommb", &sh.custom_mb, 1, 4000, "%d MB", -1))
			mark_settings_dirty();

		ImGui::Dummy(ImVec2(0, px(2)));
		text(fonts.body_bold, col::text, "Video format");
		struct C {
			const char *id, *name, *desc;
		} codecs[] = {{"h264", "H.264", "Plays everywhere"},
			      {"hevc", "HEVC", "Smaller, most devices"},
			      {"av1", "AV1", "Smallest, newer devices"}};
		float cw = (left_w - px(12)) / 3;
		for (int i = 0; i < 3; i++) {
			if (i)
				ImGui::SameLine(0, px(6));
			ImVec2 p = ImGui::GetCursorScreenPos();
			ImGui::PushID(i);
			bool clicked = ImGui::InvisibleButton("##c", ImVec2(cw, px(56)));
			bool hov = ImGui::IsItemHovered();
			ImGui::PopID();
			bool sel = sh.codec == codecs[i].id;
			dl->AddRectFilled(p, ImVec2(p.x + cw, p.y + px(56)), sel ? col::bg3 : col::bg0, px(10));
			dl->AddRect(p, ImVec2(p.x + cw, p.y + px(56)), sel ? col::accent : (hov ? col::key_border : col::line), px(10),
				    0, px(1));
			dl->AddText(fonts.mono_bold, fonts.mono_bold->FontSize, ImVec2(p.x + px(10), p.y + px(9)),
				    sel ? col::accent_text : col::text, codecs[i].name);
			text_ellipsis(dl, fonts.caption, ImVec2(p.x + px(10), p.y + px(31)), cw - px(16), col::muted, codecs[i].desc);
			if (clicked) {
				sh.codec = codecs[i].id;
				mark_settings_dirty();
			}
		}
		ImGui::EndChild();
		ImGui::PopItemWidth();
		ImGui::EndGroup();

		if (two)
			ImGui::SameLine(0, px(24));

		ImGui::BeginGroup();
		ImGui::BeginChild("##right", ImVec2(right_w, 0), ImGuiChildFlags_AutoResizeY);
		{
			/* estimate card */
			ImVec2 p = ImGui::GetCursorScreenPos();
			const float ch = px(196);
			dl = ImGui::GetWindowDrawList();
			dl->AddRectFilled(p, ImVec2(p.x + right_w, p.y + ch), col::bg0, px(14));
			dl->AddRect(p, ImVec2(p.x + right_w, p.y + ch), col::line, px(14), 0, px(1));
			float x = p.x + px(18), y = p.y + px(16);
			dl->AddText(fonts.mono_small, fonts.mono_small->FontSize, ImVec2(x, y), col::dim, "SHARE COPY");
			std::string size = "≈ " + hl::format_bytes((uint64_t)plan.estimated_bytes);
			dl->AddText(fonts.big, fonts.big->FontSize, ImVec2(x, y + px(20)), col::text, size.c_str());
			float sw = measure(fonts.big, size.c_str()).x;
			int smaller = src_est > 0 ? (int)std::round((1.0 - plan.estimated_bytes / src_est) * 100) : 0;
			if (smaller > 0) {
				std::string s = std::to_string(smaller) + "% smaller";
				dl->AddText(fonts.body, fonts.body->FontSize, ImVec2(x + sw + px(10), y + px(40)), col::muted, s.c_str());
			}
			std::string codec_name = plan.codec == "hevc" ? "HEVC" : plan.codec == "av1" ? "AV1" : "H.264";
			std::string spec = std::to_string(plan.height) + "p · " + std::to_string(plan.fps) + " fps · " + codec_name +
					   (plan.video_bitrate > 0 ? " · 2-pass on GPU" : " · constant quality");
			dl->AddText(fonts.mono, fonts.mono->FontSize, ImVec2(x, y + px(70)), col::text, spec.c_str());

			dl->AddText(fonts.caption, fonts.caption->FontSize, ImVec2(x, y + px(98)), col::muted, "Looks");
			ImU32 qc = plan.quality_level <= 1 ? col::muted : col::accent_text;
			ImVec2 qs = measure(fonts.small_semibold, plan.quality.c_str());
			dl->AddText(fonts.small_semibold, fonts.small_semibold->FontSize,
				    ImVec2(p.x + right_w - px(18) - qs.x, y + px(98)), qc, plan.quality.c_str());
			float bw = (right_w - px(36) - px(12)) / 4;
			for (int i = 0; i < 4; i++) {
				ImU32 c = i < plan.quality_level ? (plan.quality_level == 1 ? col::muted : col::accent) : col::bg3;
				dl->AddRectFilled(ImVec2(x + i * (bw + px(4)), y + px(118)),
						  ImVec2(x + i * (bw + px(4)) + bw, y + px(124)), c, px(3));
			}
			const char *note = plan.quality_level >= 4   ? "You will not see a difference from the original."
					   : plan.quality_level == 3 ? "Sharp. Fine detail holds up in fast motion."
					   : plan.quality_level == 2 ? "Clean at this size. The resolution was lowered to keep motion smooth."
								     : "Too long for this limit. Trim it shorter or pick a bigger limit.";
			dl->AddText(fonts.caption, fonts.caption->FontSize, ImVec2(x, y + px(134)), col::muted, note, nullptr,
				    right_w - px(36));
			ImGui::Dummy(ImVec2(right_w, ch));
		}
		const bool links_ready = R2Config::from(sh).missing().empty();
		if (links_ready && toggle_row("Upload and copy a link",
					      "To your Cloudflare storage. The link plays in any browser and right in Discord.",
					      &sh.upload_link))
			mark_settings_dirty();
		if (!(links_ready && sh.upload_link) &&
		    toggle_row("Copy to clipboard when done", "Paste straight into Discord with Ctrl+V.", &sh.copy_to_clipboard))
			mark_settings_dirty();
		if (!links_ready) {
			ImGui::PushStyleColor(ImGuiCol_Text, col::accent_text);
			ImGui::PushFont(fonts.small_semibold);
			if (ImGui::Selectable("Share a link instead of a file: set up share links##setuplinks", false, 0,
					      ImVec2(right_w, 0))) {
				ImGui::CloseCurrentPopup();
				share_open_ = false;
				page_ = Page::Links; /* the editor stays open under Clips */
			}
			ImGui::PopFont();
			ImGui::PopStyleColor();
		}
		bool cleanup = settings_.storage.share_cleanup_days > 0;
		if (toggle_row("Delete share copies after 7 days", "They live in Clips\\_Share. Originals are never deleted.",
			       &cleanup)) {
			settings_.storage.share_cleanup_days = cleanup ? 7 : 0;
			mark_settings_dirty();
		}
		text_wrapped(fonts.caption, col::muted, right_w, "Your original stays exactly as it is: full size, full quality.");
		if (secondary_button("Cancel##share", ImVec2(px(110), px(48)))) {
			ImGui::CloseCurrentPopup();
			share_open_ = false;
		}
		ImGui::SameLine(0, px(8));
		if (primary_button("CREATE SHARE COPY##go", ImVec2(-1, px(48)))) {
			flush_settings(true);
			std::string dir = settings_.share_dir();
			std::error_code ec;
			fs::create_directories(hl::widen(dir), ec);
			std::string stem = hl::narrow(fs::path(hl::widen(e.clip.path)).stem().wstring());
			fs::path out = fs::path(hl::widen(dir)) / hl::widen(stem + "_share.mp4");
			for (int i = 2; fs::exists(out, ec) && i < 1000; i++)
				out = fs::path(hl::widen(dir)) / hl::widen(stem + "_share (" + std::to_string(i) + ").mp4");
			req.output = hl::narrow(out.wstring());

			const bool upload = links_ready && sh.upload_link;
			const R2Config cfg = R2Config::from(sh);
			const std::string key = upload ? r2_new_key(e.clip.name, req.output) : std::string();
			share_job_.join();
			share_job_.progress = 0;
			share_job_.cancel = false;
			share_job_.phase = 0;
			share_job_.running = true;
			{
				std::lock_guard lock(share_job_.mutex);
				share_job_.done = false;
				share_job_.ok = false;
				share_job_.error.clear();
				share_job_.link.clear();
				share_job_.upload_error.clear();
				share_job_.key = key;
				share_job_.source = e.clip.path;
				share_job_.handled = false;
			}
			const bool clip = sh.copy_to_clipboard && !upload;
			share_job_.thread = std::thread([this, req, plan, clip, upload, cfg, key]() {
				std::string err, upload_err, link;
				int64_t bytes = 0;
				auto progress = [this](float p) {
					share_job_.progress = p;
					return !share_job_.cancel.load();
				};
				bool ok = hl::media::run_share(req, plan, progress, &err, &bytes);
				if (ok && clip)
					hl::media::copy_file_to_clipboard(req.output);
				if (ok && upload) {
					/* the link is copied and saved by finish_jobs() on the UI thread */
					share_job_.phase = 1;
					share_job_.progress = 0;
					if (r2_upload(cfg, req.output, key, progress, &upload_err))
						link = cfg.link(key);
				}
				std::lock_guard lock(share_job_.mutex);
				share_job_.ok = ok;
				share_job_.error = err;
				share_job_.result = req.output;
				share_job_.bytes = bytes;
				share_job_.link = link;
				share_job_.upload_error = upload_err;
				share_job_.done = true;
				share_job_.running = false;
			});
		}
		ImGui::EndChild();
		ImGui::EndGroup();
	} else if (running) {
		ImGui::Dummy(ImVec2(0, px(20)));
		const bool uploading = share_job_.phase == 1;
		text(fonts.title, col::text, uploading ? "Uploading your clip…" : "Making your share copy…");
		text(fonts.body, col::muted,
		     uploading ? "To your Cloudflare storage. The link is copied when it is done." : "Encoding on your GPU. You can keep playing.");
		ImVec2 p = ImGui::GetCursorScreenPos();
		float pr = share_job_.progress.load();
		dl->AddRectFilled(p, ImVec2(p.x + inner, p.y + px(8)), col::bg0, px(4));
		dl->AddRectFilled(p, ImVec2(p.x + inner * pr, p.y + px(8)), col::accent, px(4));
		ImGui::Dummy(ImVec2(inner, px(10)));
		text(fonts.mono_small, col::muted, "%d%%", (int)(pr * 100));
		if (secondary_button("Cancel##sharerun", ImVec2(px(120), px(44))))
			share_job_.cancel = true;
		ImGui::Dummy(ImVec2(0, px(12)));
	} else {
		std::lock_guard lock(share_job_.mutex);
		ImGui::Dummy(ImVec2(0, px(16)));
		if (share_job_.ok) {
			ImVec2 p = ImGui::GetCursorScreenPos();
			ImVec2 ic(p.x + inner / 2, p.y + px(32));
			dl->AddRectFilled(ImVec2(ic.x - px(32), ic.y - px(32)), ImVec2(ic.x + px(32), ic.y + px(32)), col::accent,
					  px(16));
			draw_icon(dl, Icon::Check, ic, px(30), col::ink);
			ImGui::Dummy(ImVec2(inner, px(76)));
			const bool linked = !share_job_.link.empty();
			const bool upload_failed = !share_job_.upload_error.empty();
			const bool copied = linked || (settings_.share.copy_to_clipboard && !upload_failed);
			const char *head = linked	   ? "Link copied. Paste it anywhere."
					   : upload_failed ? "Share copy ready, but the upload failed."
					   : copied	   ? "Copied. Paste it anywhere."
							   : "Share copy ready.";
			ImVec2 hs = measure(fonts.title, head);
			ImGui::SetCursorPosX((ImGui::GetWindowWidth() - hs.x) / 2);
			text(fonts.title, col::text, "%s", head);
			std::string line = linked ? "It plays in any browser and right in Discord. The file is " +
							    hl::format_bytes((uint64_t)share_job_.bytes) + "."
					   : upload_failed ? share_job_.upload_error
					   : copied	   ? "Open the chat and press Ctrl + V. The file is " +
								 hl::format_bytes((uint64_t)share_job_.bytes) + "."
							   : "The file is " + hl::format_bytes((uint64_t)share_job_.bytes) + ".";
			if (measure(fonts.body, line.c_str()).x > inner) {
				text_wrapped(fonts.body, col::muted, inner, "%s", line.c_str());
			} else {
				ImVec2 ls = measure(fonts.body, line.c_str());
				ImGui::SetCursorPosX((ImGui::GetWindowWidth() - ls.x) / 2);
				text(fonts.body, col::muted, "%s", line.c_str());
			}
			const std::string &shown = linked ? share_job_.link : share_job_.result;
			ImVec2 ps = measure(fonts.mono_small, shown.c_str());
			ImGui::SetCursorPosX(std::max(px(24), (ImGui::GetWindowWidth() - ps.x) / 2));
			text(fonts.mono_small, linked ? col::accent_text : col::muted, "%s", shown.c_str());
			ImGui::Dummy(ImVec2(0, px(6)));
			float bw = px(160);
			ImGui::SetCursorPosX((ImGui::GetWindowWidth() - bw * 3 - px(16)) / 2);
			if (linked) {
				if (secondary_button("Copy link##copylink", ImVec2(bw, px(48)), Icon::Share))
					copy_link(share_job_.link);
			} else if (upload_failed) {
				if (secondary_button("Try again##reupload", ImVec2(bw, px(48)), Icon::Share)) {
					start_upload(share_job_.result, share_job_.source, e.clip.name);
					share_job_.done = false;
					ImGui::CloseCurrentPopup();
					share_open_ = false;
				}
			} else if (secondary_button("Change settings##again", ImVec2(bw, px(48)))) {
				share_job_.done = false;
			}
			ImGui::SameLine(0, px(8));
			if (linked) {
				if (secondary_button("Open link##openlink", ImVec2(bw, px(48))))
					ShellExecuteW(nullptr, L"open", hl::widen(share_job_.link).c_str(), nullptr, nullptr, SW_SHOWNORMAL);
			} else if (secondary_button("Show in folder##sharefolder", ImVec2(bw, px(48)), Icon::Folder)) {
				show_in_folder(share_job_.result);
			}
			ImGui::SameLine(0, px(8));
			if (primary_button("DONE##sharedone", ImVec2(bw, px(48)))) {
				share_job_.done = false;
				ImGui::CloseCurrentPopup();
				share_open_ = false;
			}
		} else {
			text(fonts.title, col::text, "The share copy failed");
			text_wrapped(fonts.body, col::muted, inner, "%s", share_job_.error.c_str());
			if (secondary_button("Back##sharefail", ImVec2(px(120), px(44))))
				share_job_.done = false;
		}
		ImGui::Dummy(ImVec2(0, px(12)));
	}

	ImGui::EndPopup();
	ImGui::PopStyleVar(3);
	ImGui::PopStyleColor(2);
}

} // namespace ui

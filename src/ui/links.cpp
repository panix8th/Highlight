/* Share links: uploading clips to the user's Cloudflare R2 bucket and keeping
 * track of their links (the R2 client itself is r2.cpp). */

#include "app_ui.h"
#include "r2.h"

#include "../common/strings.h"

#include <algorithm>
#include <ctime>
#include <filesystem>

namespace ui {

namespace fs = std::filesystem;

void AppUI::copy_link(const std::string &link)
{
	if (copy_text_to_clipboard(link))
		notice("Link copied. Paste it anywhere.");
	else
		notice("Could not copy the link", true);
}

void AppUI::start_upload(const std::string &file, const std::string &clip_path, const std::string &name)
{
	if (upload_job_.running) {
		notice("Another upload is still running", true);
		return;
	}
	const R2Config cfg = R2Config::from(settings_.share);
	if (!cfg.missing().empty()) {
		page_ = Page::Links;
		notice("Set up share links first: your Cloudflare storage is not connected yet.");
		return;
	}
	const std::string key = r2_new_key(name, file);
	upload_job_.join();
	upload_job_.progress = 0;
	upload_job_.cancel = false;
	upload_job_.phase = 0;
	upload_job_.running = true;
	{
		std::lock_guard lock(upload_job_.mutex);
		upload_job_.done = false;
		upload_job_.ok = false;
		upload_job_.error.clear();
		upload_job_.link.clear();
		upload_job_.key = key;
		upload_job_.source = clip_path;
		upload_job_.handled = false;
	}
	upload_name_ = name;
	upload_job_.thread = std::thread([this, cfg, file, key]() {
		std::string err;
		std::error_code ec;
		const uint64_t bytes = fs::file_size(hl::widen(file), ec);
		const bool ok = r2_upload(
			cfg, file, key,
			[this](float p) {
				upload_job_.progress = p;
				return !upload_job_.cancel.load();
			},
			&err);
		std::lock_guard lock(upload_job_.mutex);
		upload_job_.ok = ok;
		upload_job_.error = err;
		upload_job_.link = ok ? cfg.link(key) : std::string();
		upload_job_.bytes = (int64_t)bytes;
		upload_job_.done = true;
		upload_job_.running = false;
	});
}

void AppUI::start_unlink(const std::string &clip_path)
{
	const hl::Upload *up = library_.upload(clip_path);
	if (!up)
		return;
	if (upload_job_.running) {
		notice("Wait for the upload to finish", true);
		return;
	}
	const R2Config cfg = R2Config::from(settings_.share);
	const std::string key = up->key;
	upload_job_.join();
	upload_job_.progress = 0;
	upload_job_.cancel = false;
	upload_job_.phase = 2;
	upload_job_.running = true;
	{
		std::lock_guard lock(upload_job_.mutex);
		upload_job_.done = false;
		upload_job_.ok = false;
		upload_job_.error.clear();
		upload_job_.link.clear();
		upload_job_.key = key;
		upload_job_.source = clip_path;
		upload_job_.handled = false;
	}
	upload_job_.thread = std::thread([this, cfg, key]() {
		std::string err;
		const bool ok = r2_delete(cfg, key, &err);
		std::lock_guard lock(upload_job_.mutex);
		upload_job_.ok = ok;
		upload_job_.error = err;
		upload_job_.done = true;
		upload_job_.running = false;
	});
}

void AppUI::finish_jobs()
{
	/* upload / delete from the clip menu */
	{
		std::string link, key, source, err;
		bool ok = false, picked = false;
		int64_t bytes = 0;
		{
			std::lock_guard lock(upload_job_.mutex);
			if (upload_job_.done && !upload_job_.handled) {
				upload_job_.handled = picked = true;
				ok = upload_job_.ok;
				link = upload_job_.link;
				key = upload_job_.key;
				source = upload_job_.source;
				err = upload_job_.error;
				bytes = upload_job_.bytes;
			}
		}
		if (picked && upload_job_.phase == 2) {
			if (ok) {
				library_.remove_upload(source);
				library_.save();
				notice("Link deleted. It no longer opens.");
			} else {
				notice("Could not delete the link: " + err, true);
			}
		} else if (picked && ok) {
			library_.set_upload(source, {key, link, (uint64_t)bytes, (int64_t)time(nullptr)});
			library_.save();
			copy_link(link);
		} else if (picked && err != "Cancelled") {
			notice("Upload failed: " + err, true);
		}
	}
	/* upload at the end of a share copy */
	{
		std::string link, key, source;
		int64_t bytes = 0;
		{
			std::lock_guard lock(share_job_.mutex);
			if (share_job_.done && !share_job_.handled) {
				share_job_.handled = true;
				link = share_job_.link;
				key = share_job_.key;
				source = share_job_.source;
				bytes = share_job_.bytes;
			}
		}
		if (!link.empty()) {
			library_.set_upload(source, {key, link, (uint64_t)bytes, (int64_t)time(nullptr)});
			library_.save();
			copy_text_to_clipboard(link);
		}
	}
}

void AppUI::draw_upload_bar()
{
	if (!upload_job_.running)
		return;
	const ImGuiViewport *vp = ImGui::GetMainViewport();
	const float w = std::min(px(380), vp->Size.x - px(40)), h = px(64);
	ImGui::SetNextWindowPos(ImVec2(vp->Pos.x + vp->Size.x - w - px(24), vp->Pos.y + vp->Size.y - h - px(24)));
	ImGui::SetNextWindowSize(ImVec2(w, h));
	ImGui::PushStyleColor(ImGuiCol_WindowBg, col::bg1);
	ImGui::PushStyleColor(ImGuiCol_Border, col::line);
	ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, px(12));
	ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, px(1));
	ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(px(16), px(12)));
	if (ImGui::Begin("##uploadbar", nullptr,
			 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
				 ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav)) {
		ImDrawList *dl = ImGui::GetWindowDrawList();
		const ImVec2 p = ImGui::GetCursorScreenPos();
		const float inner = w - px(32) - px(40);
		const bool deleting = upload_job_.phase == 2;
		const float pr = upload_job_.progress.load();
		std::string line = deleting ? "Deleting the link…" : "Uploading “" + upload_name_ + "”";
		text_ellipsis(dl, fonts.body_medium, p, inner - (deleting ? 0 : px(44)), col::text, line);
		if (!deleting) {
			std::string pct = std::to_string((int)(pr * 100)) + "%";
			ImVec2 ps = measure(fonts.mono_small, pct.c_str());
			dl->AddText(fonts.mono_small, fonts.mono_small->FontSize, ImVec2(p.x + inner - ps.x, p.y + px(2)), col::muted,
				    pct.c_str());
		}
		const ImVec2 b(p.x, p.y + px(28));
		dl->AddRectFilled(b, ImVec2(b.x + inner, b.y + px(6)), col::bg3, px(3));
		dl->AddRectFilled(b, ImVec2(b.x + inner * (deleting ? 1.0f : pr), b.y + px(6)), col::accent, px(3));
		ImGui::SetCursorScreenPos(ImVec2(p.x + inner + px(4), p.y));
		if (!deleting && icon_button("##cancelupload", Icon::Close, px(36), "Cancel upload"))
			upload_job_.cancel = true;
	}
	ImGui::End();
	ImGui::PopStyleVar(3);
	ImGui::PopStyleColor(2);
}

} // namespace ui

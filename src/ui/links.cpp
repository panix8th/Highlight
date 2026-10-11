/* Share links: uploading clips to the user's Cloudflare R2 bucket and keeping
 * track of their links (the R2 client itself is r2.cpp). */

#include "app_ui.h"
#include "r2.h"

#include "../common/strings.h"
#include "../media/remux.h"

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

bool AppUI::start_upload(const std::string &file, const std::string &clip_path, const std::string &name)
{
	if (upload_job_.running) {
		notice("Another upload is still running", true);
		return false;
	}
	const R2Config cfg = R2Config::from(settings_.share);
	if (!cfg.missing().empty()) {
		page_ = Page::Links;
		notice("Set up share links first: your Cloudflare storage is not connected yet.");
		return false;
	}
	/* MKV and MOV links would download instead of playing in browsers and Discord:
	 * those clips go up as MP4 (same video and audio, new container) */
	const std::string ext = hl::to_lower(hl::narrow(fs::path(hl::widen(file)).extension().wstring()));
	const bool to_mp4 = ext == ".mkv" || ext == ".mov";
	const std::string key = r2_new_key(name, to_mp4 ? name + ".mp4" : file);
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
		upload_job_.host = cfg.host;
		upload_job_.bucket = cfg.bucket;
		upload_job_.source = clip_path;
		upload_job_.handled = false;
	}
	upload_name_ = name;
	upload_job_.thread = std::thread([this, cfg, file, key, to_mp4]() {
		std::string err, upload = file;
		bool ok = true;
		if (to_mp4) {
			wchar_t tmp[MAX_PATH] = {};
			GetTempPathW(MAX_PATH, tmp);
			upload = hl::narrow((fs::path(tmp) / hl::widen("Highlight-upload-" + std::to_string(GetCurrentProcessId()) +
								       ".mp4"))
						    .wstring());
			ok = hl::media::remux(file, upload, "mp4", &err);
			if (!ok)
				err = "Could not prepare the clip as MP4: " + err;
		}
		std::error_code ec;
		const uint64_t bytes = fs::file_size(hl::widen(upload), ec);
		if (ok)
			ok = r2_upload(
				cfg, upload, key,
				[this](float p) {
					upload_job_.progress = p;
					return !upload_job_.cancel.load();
				},
				&err);
		if (to_mp4)
			fs::remove(hl::widen(upload), ec);
		std::lock_guard lock(upload_job_.mutex);
		upload_job_.ok = ok;
		upload_job_.error = err;
		upload_job_.link = ok ? cfg.link(key) : std::string();
		upload_job_.bytes = (int64_t)bytes;
		upload_job_.done = true;
		upload_job_.running = false;
	});
	return true;
}

/* The settings, pointed at the endpoint and bucket an upload went to. */
static R2Config config_for(const hl::ShareSettings &s, const hl::Upload &u)
{
	R2Config cfg = R2Config::from(s);
	if (!u.host.empty())
		cfg.host = u.host;
	if (!u.bucket.empty())
		cfg.bucket = u.bucket;
	return cfg;
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
	const R2Config cfg = config_for(settings_.share, *up);
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

void AppUI::start_delete_older()
{
	if (upload_job_.running || library_.older_uploads().empty())
		return;
	std::vector<std::pair<R2Config, std::string>> todo;
	for (const auto &u : library_.older_uploads())
		todo.push_back({config_for(settings_.share, u), u.key});
	upload_job_.join();
	upload_job_.progress = 0;
	upload_job_.cancel = false;
	upload_job_.phase = 3;
	upload_job_.running = true;
	{
		std::lock_guard lock(upload_job_.mutex);
		upload_job_.done = false;
		upload_job_.ok = false;
		upload_job_.error.clear();
		upload_job_.result.clear();
		upload_job_.handled = false;
	}
	upload_job_.thread = std::thread([this, todo]() {
		std::string deleted, first_error;
		for (size_t i = 0; i < todo.size() && !upload_job_.cancel; i++) {
			std::string err;
			if (r2_delete(todo[i].first, todo[i].second, &err))
				deleted += todo[i].second + "\n";
			else if (first_error.empty())
				first_error = err;
			upload_job_.progress = (float)(i + 1) / (float)todo.size();
		}
		std::lock_guard lock(upload_job_.mutex);
		upload_job_.ok = first_error.empty();
		upload_job_.error = first_error;
		upload_job_.result = deleted; /* object names, one per line */
		upload_job_.done = true;
		upload_job_.running = false;
	});
}

void AppUI::record_upload(const std::string &clip_path, const hl::Upload &u)
{
	std::error_code ec;
	if (fs::exists(hl::widen(clip_path), ec))
		library_.set_upload(clip_path, u);
	else
		library_.add_older(u); /* the clip was deleted or renamed while it uploaded */
	library_.save();
}

void AppUI::finish_jobs()
{
	/* upload / delete from the clip menu, or deleting older links */
	{
		hl::Upload up;
		std::string source, err, deleted;
		bool ok = false, picked = false;
		{
			std::lock_guard lock(upload_job_.mutex);
			if (upload_job_.done && !upload_job_.handled) {
				upload_job_.handled = picked = true;
				ok = upload_job_.ok;
				up = {upload_job_.key, upload_job_.link, upload_job_.host, upload_job_.bucket,
				      (uint64_t)upload_job_.bytes, (int64_t)time(nullptr)};
				source = upload_job_.source;
				err = upload_job_.error;
				deleted = upload_job_.result;
			}
		}
		const int phase = upload_job_.phase;
		if (picked && phase == 3) {
			size_t n = 0;
			for (size_t a = 0, b; (b = deleted.find('\n', a)) != std::string::npos; a = b + 1, n++)
				library_.remove_older(deleted.substr(a, b - a));
			library_.save();
			if (ok)
				notice(std::to_string(n) + (n == 1 ? " older link deleted" : " older links deleted"));
			else
				notice("Some older links could not be deleted: " + err, true);
		} else if (picked && phase == 2) {
			if (ok) {
				library_.remove_upload(source);
				library_.save();
				notice("Link deleted. It no longer opens.");
			} else {
				notice("Could not delete the link: " + err, true);
			}
		} else if (picked && ok) {
			record_upload(source, up);
			copy_link(up.link);
		} else if (picked && err != "Cancelled") {
			notice("Upload failed: " + err, true);
		}
	}
	/* upload at the end of a share copy */
	{
		hl::Upload up;
		std::string source;
		{
			std::lock_guard lock(share_job_.mutex);
			if (share_job_.done && !share_job_.handled) {
				share_job_.handled = true;
				up = {share_job_.key, share_job_.link, share_job_.host, share_job_.bucket,
				      (uint64_t)share_job_.bytes, (int64_t)time(nullptr)};
				source = share_job_.source;
			}
		}
		if (!up.link.empty()) {
			record_upload(source, up);
			copy_link(up.link);
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
		const int phase = upload_job_.phase;
		const bool deleting = phase == 2;
		const float pr = upload_job_.progress.load();
		std::string line = phase == 3 ? "Deleting older links…"
				   : deleting ? "Deleting the link…"
					      : "Uploading “" + upload_name_ + "”";
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

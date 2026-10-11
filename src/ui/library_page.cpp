#include "app_ui.h"

#include "../common/strings.h"

#include <shellapi.h>

#include <algorithm>
#include <ctime>
#include <filesystem>
#include <map>

namespace ui {

namespace fs = std::filesystem;

static ImU32 tone_for(const std::string &s)
{
	static const ImU32 tones[] = {IM_COL32(0x3B, 0x1E, 0x25, 255), IM_COL32(0x2E, 0x2B, 0x1C, 255),
				      IM_COL32(0x3A, 0x25, 0x17, 255), IM_COL32(0x18, 0x2B, 0x3B, 255),
				      IM_COL32(0x1B, 0x2B, 0x25, 255), IM_COL32(0x2A, 0x1C, 0x22, 255)};
	uint32_t h = 2166136261u;
	for (unsigned char c : s)
		h = (h ^ c) * 16777619u;
	return tones[h % (sizeof(tones) / sizeof(tones[0]))];
}

static std::string clock_text(int64_t t)
{
	time_t tt = (time_t)t;
	tm lt;
	localtime_s(&lt, &tt);
	char buf[16];
	strftime(buf, sizeof(buf), "%H:%M", &lt);
	return buf;
}

static int day_index(int64_t t)
{
	time_t tt = (time_t)t;
	tm lt;
	localtime_s(&lt, &tt);
	return lt.tm_year * 400 + lt.tm_yday;
}

static std::string group_name(int64_t t)
{
	time_t now = time(nullptr);
	int today = day_index((int64_t)now);
	int d = day_index(t);
	if (d == today)
		return "Today";
	if (d == today - 1)
		return "Yesterday";
	if (today - d < 7)
		return "This week";
	time_t tt = (time_t)t;
	tm lt;
	localtime_s(&lt, &tt);
	char buf[32];
	strftime(buf, sizeof(buf), "%B %Y", &lt);
	return buf;
}

void AppUI::library_page()
{
	std::vector<ClipItem> clips;
	uint64_t total;
	{
		std::lock_guard lock(clips_mutex_);
		clips = clips_;
		total = clips_total_bytes_;
	}

	/* header */
	const float avail = ImGui::GetContentRegionAvail().x;
	ImVec2 start = ImGui::GetCursorPos();
	size_t video_count = std::count_if(clips.begin(), clips.end(), [](const ClipItem &c) { return !c.screenshot; });
	std::string sub = std::to_string(video_count) + (video_count == 1 ? " clip · " : " clips · ") +
			  hl::format_bytes(total) + " · saved to " + settings_.clips_dir();
	page_header("Clips", sub.c_str());
	ImVec2 after = ImGui::GetCursorPos();

	const float search_w = px(260), btn_w = px(140);
	if (avail > px(760)) {
		ImGui::SetCursorPos(ImVec2(start.x + avail - search_w - btn_w - px(8), start.y + px(14)));
	}
	{
		std::string s = search_;
		if (input_text("##search", s, search_w, "Search clips, games, dates"))
			snprintf(search_, sizeof(search_), "%s", s.c_str());
		ImGui::SameLine(0, px(8));
		if (secondary_button("Open folder##lib", ImVec2(btn_w, px(44)), Icon::Folder))
			send({{"cmd", "open_folder"}});
	}
	if (avail > px(760))
		ImGui::SetCursorPos(after);

	/* filter chips */
	std::map<std::string, int> games;
	for (const auto &c : clips)
		if (!c.screenshot)
			games[c.game]++;
	std::vector<std::pair<int, std::string>> top;
	for (auto &[g, n] : games)
		top.emplace_back(-n, g);
	std::sort(top.begin(), top.end());
	std::vector<std::string> chips = {"All", "Favorites", "Sessions", "Screenshots"};
	for (size_t i = 0; i < top.size() && i < 6; i++)
		chips.push_back(top[i].second);

	ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(px(8), px(8)));
	float x = 0;
	for (size_t i = 0; i < chips.size(); i++) {
		float w = measure(fonts.small_semibold, chips[i].c_str()).x + px(32);
		if (i > 0) {
			if (x + w + px(8) < avail)
				ImGui::SameLine();
			else
				x = 0;
		}
		std::string id = chips[i] + "##chip" + std::to_string(i);
		if (pill(id.c_str(), filter_ == chips[i], ImVec2(w, px(40))))
			filter_ = chips[i];
		x += w + px(8);
	}
	ImGui::PopStyleVar();

	ImGui::SameLine();
	{
		static const std::vector<std::string> sorts = {"Newest", "Oldest", "Largest", "Longest"};
		float cw = px(140);
		float cx = ImGui::GetCursorPosX();
		float rem = ImGui::GetContentRegionAvail().x;
		if (rem > cw + px(8)) {
			ImGui::SetCursorPosX(cx + rem - cw);
			combo("##sort", &sort_, sorts, cw);
		} else {
			ImGui::NewLine();
		}
	}

	/* filter + sort */
	std::string q = hl::to_lower(search_);
	std::vector<ClipItem> shown;
	for (const auto &c : clips) {
		if (filter_ == "Favorites" && !library_.is_favorite(c.path))
			continue;
		if (filter_ == "Sessions" && !c.session)
			continue;
		if (filter_ == "Screenshots" && !c.screenshot)
			continue;
		if (filter_ != "All" && filter_ != "Favorites" && filter_ != "Sessions" && filter_ != "Screenshots" &&
		    c.game != filter_)
			continue;
		if (filter_ != "Screenshots" && filter_ != "All" && filter_ != "Favorites" && c.screenshot)
			continue;
		if (!q.empty()) {
			std::string hay = hl::to_lower(c.name + " " + c.game + " " + c.path + " " + group_name(c.mtime));
			if (hay.find(q) == std::string::npos)
				continue;
		}
		shown.push_back(c);
	}
	if (sort_ == 1)
		std::reverse(shown.begin(), shown.end());
	else if (sort_ == 2)
		std::stable_sort(shown.begin(), shown.end(), [](auto &a, auto &b) { return a.size > b.size; });
	else if (sort_ == 3)
		std::stable_sort(shown.begin(), shown.end(), [](auto &a, auto &b) { return a.duration > b.duration; });

	if (shown.empty()) {
		ImGui::Dummy(ImVec2(0, px(8)));
		begin_card("##empty", avail, true);
		if (clips.empty()) {
			text(fonts.h2, col::text, "No clips yet");
			text_wrapped(fonts.body, col::muted, avail - px(40), "Play a game and press %s to save the last %s.",
				     settings_.hotkeys.save.to_string().c_str(),
				     hl::format_duration(settings_.clipping.clip_seconds).c_str());
		} else {
			text(fonts.h2, col::text, "Nothing matches");
			text(fonts.body, col::muted, "Try another filter or search.");
		}
		end_card();
		clip_menu();
		return;
	}

	/* grid, grouped by day when sorted by date */
	const float gap = px(16);
	const float min_w = px(220);
	int cols = std::max(1, (int)((avail + gap) / (min_w + gap)));
	float card_w = (avail - gap * (cols - 1)) / cols;
	bool by_date = sort_ == 0 || sort_ == 1;

	size_t i = 0;
	while (i < shown.size()) {
		std::string group = by_date ? group_name(shown[i].mtime) : "";
		size_t j = i;
		while (j < shown.size() && (!by_date || group_name(shown[j].mtime) == group))
			j++;
		if (by_date) {
			ImGui::Dummy(ImVec2(0, px(2)));
			std::string upper = group;
			std::transform(upper.begin(), upper.end(), upper.begin(), ::toupper);
			text(fonts.small_semibold, col::text, "%s", upper.c_str());
			ImGui::SameLine();
			text(fonts.mono_small, col::dim, "%zu", j - i);
		}
		ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(gap, gap));
		for (size_t k = i; k < j; k++) {
			if ((k - i) % cols != 0)
				ImGui::SameLine();
			ImGui::PushID(shown[k].path.c_str());
			clip_card(shown[k], card_w);
			ImGui::PopID();
		}
		ImGui::PopStyleVar();
		i = j;
	}
	clip_menu();
}

void AppUI::clip_card(const ClipItem &c, float w)
{
	ImDrawList *dl = ImGui::GetWindowDrawList();
	const float thumb_h = w * 9.0f / 16.0f;
	const float meta_h = px(66);
	ImVec2 p = ImGui::GetCursorScreenPos();
	ImVec2 q(p.x + w, p.y + thumb_h + meta_h);

	ImGui::BeginGroup();
	bool clicked = ImGui::InvisibleButton("##thumb", ImVec2(w, thumb_h));
	bool hov = ImGui::IsItemHovered();
	if (hov)
		ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
	bool visible = ImGui::IsItemVisible();

	dl->AddRectFilled(p, q, col::bg2, px(12));
	ImVec2 tq(p.x + w, p.y + thumb_h);
	const Texture *tex = visible ? thumbs_.get(c.path, c.stamp) : nullptr;
	if (tex) {
		/* cover-fit */
		float ar = (float)tex->w / tex->h, box = w / thumb_h;
		ImVec2 uv0(0, 0), uv1(1, 1);
		if (ar > box) {
			float cut = (1 - box / ar) / 2;
			uv0.x = cut;
			uv1.x = 1 - cut;
		} else {
			float cut = (1 - ar / box) / 2;
			uv0.y = cut;
			uv1.y = 1 - cut;
		}
		dl->AddImageRounded(tex->id(), p, tq, uv0, uv1, IM_COL32_WHITE, px(12), ImDrawFlags_RoundCornersTop);
	} else {
		dl->AddRectFilled(p, tq, tone_for(c.game), px(12), ImDrawFlags_RoundCornersTop);
		dl->AddRectFilled(ImVec2(p.x, p.y + thumb_h * 0.62f), tq, IM_COL32(0, 0, 0, 70));
	}
	dl->AddRect(p, q, hov ? col::key_border : col::line, px(12), 0, px(1));

	auto badge = [&](ImVec2 at, const std::string &s, bool right) {
		ImVec2 ts = measure(fonts.mono_small, s.c_str());
		ImVec2 a = right ? ImVec2(at.x - ts.x - px(14), at.y) : at;
		dl->AddRectFilled(a, ImVec2(a.x + ts.x + px(14), a.y + ts.y + px(6)), IM_COL32(0x0B, 0x0C, 0x0E, 210), px(5));
		dl->AddText(fonts.mono_small, fonts.mono_small->FontSize, ImVec2(a.x + px(7), a.y + px(3)), col::text, s.c_str());
	};
	std::string spec = c.screenshot ? "PNG"
			   : c.session    ? "SESSION"
			   : c.height > 0 ? std::to_string(c.height) + "p" + std::to_string((int)(c.fps + 0.5))
					  : "";
	if (!spec.empty())
		badge(ImVec2(p.x + px(8), p.y + px(8)), spec, false);
	if (c.duration > 0)
		badge(ImVec2(tq.x - px(8), tq.y - px(26)), hl::format_duration(c.duration), true);
	if (c.bookmarks > 0) {
		std::string bm = std::to_string(c.bookmarks);
		ImVec2 at(p.x + px(8), tq.y - px(26));
		dl->AddRectFilled(at, ImVec2(at.x + px(40), at.y + px(20)), IM_COL32(0x0B, 0x0C, 0x0E, 210), px(5));
		draw_icon(dl, Icon::Diamond, ImVec2(at.x + px(12), at.y + px(10)), px(10), col::accent);
		dl->AddText(fonts.mono_small, fonts.mono_small->FontSize, ImVec2(at.x + px(22), at.y + px(3)), col::text,
			    bm.c_str());
	}
	if (library_.is_favorite(c.path)) {
		ImVec2 sp(tq.x - px(34), p.y + px(8));
		dl->AddRectFilled(sp, ImVec2(sp.x + px(26), sp.y + px(26)), IM_COL32(0x0B, 0x0C, 0x0E, 210), px(6));
		draw_icon(dl, Icon::StarFilled, ImVec2(sp.x + px(13), sp.y + px(13)), px(14), col::accent);
	}
	if (hov && !c.screenshot) {
		ImVec2 cc((p.x + tq.x) / 2, (p.y + tq.y) / 2);
		dl->AddCircleFilled(cc, px(22), IM_COL32(0x0B, 0x0C, 0x0E, 170));
		draw_icon(dl, Icon::Play, ImVec2(cc.x + px(1), cc.y), px(16), col::text);
	}

	/* meta */
	float mx = p.x + px(14), my = tq.y + px(12);
	text_ellipsis(dl, fonts.body_semibold, ImVec2(mx, my), w - px(64), col::text, c.name);
	std::string line = c.game + " · " + clock_text(c.mtime) + " · " + hl::format_bytes(c.size);
	text_ellipsis(dl, fonts.caption, ImVec2(mx, my + px(22)), w - px(64), col::muted, line);

	ImGui::SetCursorScreenPos(ImVec2(q.x - px(50), tq.y + px(10)));
	if (icon_button("##more", Icon::More, px(44), "More")) {
		/* The popup is drawn at page level (clip_menu) so it is not tied to this card's ID. */
		menu_path_ = c.path;
		menu_open_request_ = true;
	}
	ImGui::SetCursorScreenPos(p);
	ImGui::Dummy(ImVec2(w, thumb_h + meta_h));
	ImGui::EndGroup();

	if (clicked) {
		if (c.screenshot)
			ShellExecuteW(nullptr, L"open", hl::widen(c.path).c_str(), nullptr, nullptr, SW_SHOWNORMAL);
		else
			open_editor(c);
	}
}

void AppUI::clip_menu()
{
	if (menu_open_request_) {
		menu_open_request_ = false;
		ImGui::OpenPopup("##clipmenu");
	}

	ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(px(8), px(8)));
	ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(px(4), px(2)));
	if (ImGui::BeginPopup("##clipmenu")) {
		ClipItem item;
		bool found = false;
		{
			std::lock_guard lock(clips_mutex_);
			for (auto &c : clips_)
				if (c.path == menu_path_) {
					item = c;
					found = true;
				}
		}
		auto entry = [&](const char *label, Icon icon) {
			ImVec2 p = ImGui::GetCursorScreenPos();
			bool r = ImGui::Selectable((std::string("      ") + label).c_str(), false, 0, ImVec2(px(220), px(36)));
			draw_icon(ImGui::GetWindowDrawList(), icon, ImVec2(p.x + px(14), p.y + px(18)), px(15), col::muted);
			return r;
		};
		if (found) {
			bool fav = library_.is_favorite(item.path);
			if (!item.screenshot && entry("Open in editor", Icon::Play))
				open_editor(item);
			if (!item.screenshot && entry("Share…", Icon::Share)) {
				open_editor(item);
				open_share();
			}
			if (entry(fav ? "Remove from favourites" : "Add to favourites", fav ? Icon::StarFilled : Icon::Star)) {
				library_.set_favorite(item.path, !fav);
				library_.save();
			}
			if (entry("Show in folder", Icon::Folder))
				show_in_folder(item.path);
			if (entry("Rename…", Icon::Pencil)) {
				rename_path_ = item.path;
				snprintf(rename_buf_, sizeof(rename_buf_), "%s",
					 hl::narrow(fs::path(hl::widen(item.path)).stem().wstring()).c_str());
				open_rename_ = true;
			}
			ImGui::Separator();
			if (entry("Delete", Icon::Trash))
				delete_clip(item.path);
		}
		ImGui::EndPopup();
	}
	ImGui::PopStyleVar(2);

	if (open_rename_) {
		open_rename_ = false;
		ImGui::OpenPopup("Rename clip");
	}
	ImGui::SetNextWindowSize(ImVec2(px(440), 0));
	ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(px(20), px(20)));
	if (ImGui::BeginPopupModal("Rename clip", nullptr, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize)) {
		text(fonts.h2, col::text, "Rename clip");
		std::string v = rename_buf_;
		if (ImGui::IsWindowAppearing())
			ImGui::SetKeyboardFocusHere();
		if (input_text("##rename", v, -1))
			snprintf(rename_buf_, sizeof(rename_buf_), "%s", v.c_str());
		bool enter = ImGui::IsKeyPressed(ImGuiKey_Enter);
		if (secondary_button("Cancel##rn", ImVec2(px(120), px(44))) || ImGui::IsKeyPressed(ImGuiKey_Escape))
			ImGui::CloseCurrentPopup();
		ImGui::SameLine();
		if (primary_button("RENAME##rn", ImVec2(-1, px(44))) || enter) {
			fs::path from = hl::widen(rename_path_);
			std::string name = hl::sanitize_filename(rename_buf_);
			fs::path to = from.parent_path() / (hl::widen(name) + from.extension().wstring());
			if (to != from) {
				if (MoveFileExW(from.c_str(), to.c_str(), 0)) {
					library_.rename(rename_path_, hl::narrow(to.wstring()));
					library_.save();
					rescan_ = true;
				} else {
					notice("Could not rename (is the name taken or the file open?)", true);
				}
			}
			ImGui::CloseCurrentPopup();
		}
		ImGui::EndPopup();
	}
	ImGui::PopStyleVar();
}

} // namespace ui

#include "widgets.h"

#include <imgui_internal.h>

#include <cmath>
#include <cstdarg>
#include <cstring>

namespace ui {

/* ------------------------------------------------------------------ */
/* icons (24-unit grid, line style)                                    */

void draw_icon(ImDrawList *dl, Icon icon, ImVec2 c, float size, ImU32 color)
{
	const float u = size / 24.0f;
	const float t = std::max(1.4f, 1.9f * u);
	auto P = [&](float x, float y) { return ImVec2(c.x + (x - 12) * u, c.y + (y - 12) * u); };
	auto line = [&](float x1, float y1, float x2, float y2) { dl->AddLine(P(x1, y1), P(x2, y2), color, t); };
	auto poly = [&](std::initializer_list<ImVec2> pts, bool closed) {
		for (const auto &p : pts)
			dl->PathLineTo(P(p.x, p.y));
		dl->PathStroke(color, closed ? ImDrawFlags_Closed : ImDrawFlags_None, t);
	};

	switch (icon) {
	case Icon::Grid:
		dl->AddRect(P(3, 3), P(10, 10), color, u, 0, t);
		dl->AddRect(P(14, 3), P(21, 10), color, u, 0, t);
		dl->AddRect(P(14, 14), P(21, 21), color, u, 0, t);
		dl->AddRect(P(3, 14), P(10, 21), color, u, 0, t);
		break;
	case Icon::Scissors:
		dl->AddCircle(P(6, 6), 3 * u, color, 0, t);
		dl->AddCircle(P(6, 18), 3 * u, color, 0, t);
		line(20, 4, 8.12f, 15.88f);
		line(14.47f, 14.48f, 20, 20);
		line(8.12f, 8.12f, 12, 12);
		break;
	case Icon::Monitor:
		dl->AddRect(P(2, 3), P(22, 17), color, 2 * u, 0, t);
		line(8, 21, 16, 21);
		line(12, 17, 12, 21);
		break;
	case Icon::Speaker:
	case Icon::SpeakerMute:
		poly({{11, 5}, {6, 9}, {2, 9}, {2, 15}, {6, 15}, {11, 19}}, true);
		if (icon == Icon::Speaker) {
			dl->PathArcTo(P(12, 12), 5 * u, -IM_PI / 4, IM_PI / 4);
			dl->PathStroke(color, 0, t);
			dl->PathArcTo(P(12, 12), 9 * u, -IM_PI / 4, IM_PI / 4);
			dl->PathStroke(color, 0, t);
		} else {
			line(16, 9, 22, 15);
			line(22, 9, 16, 15);
		}
		break;
	case Icon::Bell:
		dl->PathArcTo(P(12, 9), 6 * u, IM_PI, 2 * IM_PI);
		dl->PathLineTo(P(18, 15));
		dl->PathLineTo(P(20.5f, 17.5f));
		dl->PathLineTo(P(3.5f, 17.5f));
		dl->PathLineTo(P(6, 15));
		dl->PathStroke(color, ImDrawFlags_Closed, t);
		line(10, 21, 14, 21);
		break;
	case Icon::Folder:
		poly({{2, 5}, {9, 5}, {11, 8}, {22, 8}, {22, 19}, {2, 19}}, true);
		break;
	case Icon::Play:
		dl->AddTriangleFilled(P(7, 4), P(7, 20), P(20, 12), color);
		break;
	case Icon::Pause:
		dl->AddRectFilled(P(6, 4), P(10, 20), color, u);
		dl->AddRectFilled(P(14, 4), P(18, 20), color, u);
		break;
	case Icon::Star:
	case Icon::StarFilled: {
		ImVec2 pts[10];
		for (int i = 0; i < 10; i++) {
			float a = -IM_PI / 2 + i * IM_PI / 5;
			float r = (i % 2 == 0) ? 10.0f : 4.3f;
			pts[i] = P(12 + std::cos(a) * r, 12.5f + std::sin(a) * r);
		}
		if (icon == Icon::StarFilled) {
			ImVec2 mid = P(12, 12.5f);
			for (int i = 0; i < 10; i++)
				dl->AddTriangleFilled(mid, pts[i], pts[(i + 1) % 10], color);
		} else {
			dl->AddPolyline(pts, 10, color, ImDrawFlags_Closed, t);
		}
		break;
	}
	case Icon::More:
		dl->AddCircleFilled(P(5, 12), 1.7f * u, color);
		dl->AddCircleFilled(P(12, 12), 1.7f * u, color);
		dl->AddCircleFilled(P(19, 12), 1.7f * u, color);
		break;
	case Icon::Search:
		dl->AddCircle(P(11, 11), 7 * u, color, 0, t);
		line(16.2f, 16.2f, 21, 21);
		break;
	case Icon::Trash:
		line(3, 6, 21, 6);
		poly({{19, 6}, {18, 21}, {6, 21}, {5, 6}}, false);
		poly({{8.5f, 6}, {8.5f, 3}, {15.5f, 3}, {15.5f, 6}}, false);
		break;
	case Icon::Pencil:
		poly({{4, 20}, {5, 16}, {16.5f, 4.5f}, {19.5f, 7.5f}, {8, 19}}, true);
		break;
	case Icon::ChevronLeft:
		poly({{15, 6}, {9, 12}, {15, 18}}, false);
		break;
	case Icon::Minus:
		line(5, 12, 19, 12);
		break;
	case Icon::Square:
		dl->AddRect(P(5, 5), P(19, 19), color, u, 0, t);
		break;
	case Icon::Close:
		line(6, 6, 18, 18);
		line(18, 6, 6, 18);
		break;
	case Icon::Plus:
		line(12, 5, 12, 19);
		line(5, 12, 19, 12);
		break;
	case Icon::Camera:
		dl->AddRect(P(2, 7), P(22, 20), color, 2 * u, 0, t);
		poly({{8, 7}, {10, 4}, {14, 4}, {16, 7}}, false);
		dl->AddCircle(P(12, 13.5f), 3.5f * u, color, 0, t);
		break;
	case Icon::Mic:
		dl->AddRect(P(9, 2), P(15, 14), color, 3 * u, 0, t);
		dl->PathArcTo(P(12, 10), 7 * u, 0, IM_PI);
		dl->PathStroke(color, 0, t);
		line(12, 17, 12, 22);
		break;
	case Icon::Check:
		poly({{20, 6}, {9, 17}, {4, 12}}, false);
		break;
	case Icon::Share:
		poly({{4, 11}, {4, 20}, {20, 20}, {20, 11}}, false);
		line(12, 3, 12, 15);
		poly({{8, 7}, {12, 3}, {16, 7}}, false);
		break;
	case Icon::Diamond:
		dl->AddQuadFilled(P(12, 4), P(20, 12), P(12, 20), P(4, 12), color);
		break;
	case Icon::FrameBack:
		dl->AddTriangleFilled(P(19, 4), P(19, 20), P(9, 12), color);
		dl->AddRectFilled(P(5, 4), P(7.5f, 20), color);
		break;
	case Icon::FrameForward:
		dl->AddTriangleFilled(P(5, 4), P(5, 20), P(15, 12), color);
		dl->AddRectFilled(P(16.5f, 4), P(19, 20), color);
		break;
	case Icon::Record:
		dl->AddCircle(P(12, 12), 9 * u, color, 0, t);
		dl->AddCircleFilled(P(12, 12), 4 * u, color);
		break;
	case Icon::Logo: {
		const float g = size / 40.0f;
		ImVec2 o(c.x - size / 2, c.y - size / 2);
		auto L = [&](float x, float y) { return ImVec2(o.x + x * g, o.y + y * g); };
		dl->AddRectFilled(L(0, 0), L(40, 40), col::accent, 10 * g);
		dl->AddRectFilled(L(9, 10), L(14, 30), col::ink, 1.5f * g);
		dl->AddRectFilled(L(26, 10), L(31, 30), col::ink, 1.5f * g);
		dl->AddTriangleFilled(L(16.5f, 14.5f), L(16.5f, 25.5f), L(24.5f, 20), col::ink);
		break;
	}
	}
}

/* ------------------------------------------------------------------ */
/* text                                                               */

void text(ImFont *font, ImU32 color, const char *fmt, ...)
{
	ImGui::PushFont(font);
	ImGui::PushStyleColor(ImGuiCol_Text, color);
	va_list args;
	va_start(args, fmt);
	ImGui::TextV(fmt, args);
	va_end(args);
	ImGui::PopStyleColor();
	ImGui::PopFont();
}

void text_wrapped(ImFont *font, ImU32 color, float wrap_width, const char *fmt, ...)
{
	char buf[2048];
	va_list args;
	va_start(args, fmt);
	vsnprintf(buf, sizeof(buf), fmt, args);
	va_end(args);
	ImGui::PushFont(font);
	ImGui::PushStyleColor(ImGuiCol_Text, color);
	ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + wrap_width);
	ImGui::TextUnformatted(buf);
	ImGui::PopTextWrapPos();
	ImGui::PopStyleColor();
	ImGui::PopFont();
}

ImVec2 measure(ImFont *font, const char *s)
{
	return font->CalcTextSizeA(font->FontSize, FLT_MAX, 0.0f, s);
}

void text_ellipsis(ImDrawList *dl, ImFont *font, ImVec2 pos, float max_w, ImU32 color, const std::string &s)
{
	ImVec2 sz = measure(font, s.c_str());
	if (sz.x <= max_w) {
		dl->AddText(font, font->FontSize, pos, color, s.c_str());
		return;
	}
	const char *ell = "…";
	float ell_w = measure(font, ell).x;
	size_t n = s.size();
	while (n > 0) {
		/* step back over UTF-8 continuation bytes */
		do {
			n--;
		} while (n > 0 && ((unsigned char)s[n] & 0xC0) == 0x80);
		if (font->CalcTextSizeA(font->FontSize, FLT_MAX, 0.0f, s.c_str(), s.c_str() + n).x + ell_w <= max_w)
			break;
	}
	std::string cut = s.substr(0, n) + ell;
	dl->AddText(font, font->FontSize, pos, color, cut.c_str());
}

void section_label(const char *label)
{
	ImGui::Dummy(ImVec2(0, px(4)));
	ImGui::SetCursorPosX(ImGui::GetCursorPosX() + px(12));
	text(fonts.mono_small, col::dim, "%s", label);
}

/* ------------------------------------------------------------------ */
/* buttons                                                            */

static bool hovered_and_clicked(const char *id, ImVec2 size, bool enabled, bool *hovered, bool *held = nullptr)
{
	bool clicked = ImGui::InvisibleButton(id, size);
	*hovered = enabled && ImGui::IsItemHovered();
	if (held)
		*held = enabled && ImGui::IsItemActive();
	if (*hovered)
		ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
	return clicked && enabled;
}

bool primary_button(const char *label, ImVec2 size, const char *key, bool enabled)
{
	if (size.x <= 0)
		size.x = ImGui::GetContentRegionAvail().x;
	ImVec2 p = ImGui::GetCursorScreenPos();
	bool hov;
	bool clicked = hovered_and_clicked(label, size, enabled, &hov);
	ImDrawList *dl = ImGui::GetWindowDrawList();
	ImU32 bg = !enabled ? IM_COL32(0xFF, 0x5A, 0x1F, 90) : hov ? col::accent_text : col::accent;
	dl->AddRectFilled(p, ImVec2(p.x + size.x, p.y + size.y), bg, px(10));

	const char *end = ImGui::FindRenderedTextEnd(label);
	ImFont *f = fonts.wordmark;
	ImVec2 ts = f->CalcTextSizeA(f->FontSize, FLT_MAX, 0, label, end);
	if (key && *key) {
		ImVec2 ks = measure(fonts.mono_small, key);
		float kw = ks.x + px(16), kh = ks.y + px(10);
		ImVec2 kp(p.x + size.x - kw - px(8), p.y + (size.y - kh) / 2);
		dl->AddRectFilled(kp, ImVec2(kp.x + kw, kp.y + kh), col::ink, px(6));
		dl->AddText(fonts.mono_small, fonts.mono_small->FontSize, ImVec2(kp.x + px(8), kp.y + px(5)),
			    col::accent_text, key);
		dl->AddText(f, f->FontSize, ImVec2(p.x + px(14), p.y + (size.y - ts.y) / 2), col::ink, label, end);
	} else {
		dl->AddText(f, f->FontSize, ImVec2(p.x + (size.x - ts.x) / 2, p.y + (size.y - ts.y) / 2), col::ink,
			    label, end);
	}
	return clicked;
}

bool secondary_button(const char *label, ImVec2 size, Icon icon, bool enabled)
{
	const char *end = ImGui::FindRenderedTextEnd(label);
	ImFont *f = fonts.body_semibold;
	ImVec2 ts = f->CalcTextSizeA(f->FontSize, FLT_MAX, 0, label, end);
	const bool has_icon = icon != Icon::Logo;
	if (size.x <= 0)
		size.x = ts.x + px(32) + (has_icon ? px(24) : 0);
	if (size.y <= 0)
		size.y = px(44);
	ImVec2 p = ImGui::GetCursorScreenPos();
	bool hov;
	bool clicked = hovered_and_clicked(label, size, enabled, &hov);
	ImDrawList *dl = ImGui::GetWindowDrawList();
	dl->AddRectFilled(p, ImVec2(p.x + size.x, p.y + size.y), hov ? col::bg3 : col::bg2, px(10));
	dl->AddRect(p, ImVec2(p.x + size.x, p.y + size.y), col::line, px(10), 0, px(1));
	ImU32 tc = enabled ? col::text : col::dim;
	float x = p.x + (size.x - ts.x - (has_icon ? px(24) : 0)) / 2;
	if (has_icon) {
		draw_icon(dl, icon, ImVec2(x + px(8), p.y + size.y / 2), px(16), tc);
		x += px(24);
	}
	dl->AddText(f, f->FontSize, ImVec2(x, p.y + (size.y - ts.y) / 2), tc, label, end);
	return clicked;
}

bool icon_button(const char *id, Icon icon, float size, const char *tooltip, ImU32 color, bool framed)
{
	ImVec2 p = ImGui::GetCursorScreenPos();
	bool hov;
	bool clicked = hovered_and_clicked(id, ImVec2(size, size), true, &hov);
	ImDrawList *dl = ImGui::GetWindowDrawList();
	if (framed) {
		dl->AddRectFilled(p, ImVec2(p.x + size, p.y + size), hov ? col::bg3 : col::bg2, px(10));
		dl->AddRect(p, ImVec2(p.x + size, p.y + size), col::line, px(10), 0, px(1));
	} else if (hov) {
		dl->AddRectFilled(p, ImVec2(p.x + size, p.y + size), col::bg3, px(8));
	}
	draw_icon(dl, icon, ImVec2(p.x + size / 2, p.y + size / 2), std::min(size * 0.42f, px(18)),
		  hov ? col::text : color);
	if (tooltip && hov && ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal))
		ImGui::SetTooltip("%s", tooltip);
	return clicked;
}

static bool pill_impl(const char *label, bool selected, ImVec2 size, bool mono, float rounding)
{
	ImFont *f = mono ? fonts.mono : fonts.small_semibold;
	const char *end = ImGui::FindRenderedTextEnd(label);
	ImVec2 ts = f->CalcTextSizeA(f->FontSize, FLT_MAX, 0, label, end);
	if (size.x <= 0)
		size.x = ts.x + px(32);
	ImVec2 p = ImGui::GetCursorScreenPos();
	bool hov;
	bool clicked = hovered_and_clicked(label, size, true, &hov);
	ImDrawList *dl = ImGui::GetWindowDrawList();
	ImVec2 q(p.x + size.x, p.y + size.y);
	if (selected) {
		dl->AddRectFilled(p, q, col::text, rounding);
	} else {
		dl->AddRectFilled(p, q, col::bg0, rounding);
		dl->AddRect(p, q, hov ? col::key_border : col::line, rounding, 0, px(1));
	}
	dl->AddText(f, f->FontSize, ImVec2(p.x + (size.x - ts.x) / 2, p.y + (size.y - ts.y) / 2),
		    selected ? col::ink : (hov ? col::text : col::muted), label, end);
	return clicked;
}

bool pill(const char *label, bool selected, ImVec2 size, bool mono)
{
	return pill_impl(label, selected, size, mono, size.y / 2);
}

bool pill_grid(const char *id, const std::vector<std::string> &labels, int &selected, int columns, bool mono,
	       float height)
{
	ImGui::PushID(id);
	const float gap = px(8);
	const float avail = ImGui::GetContentRegionAvail().x;
	columns = std::max(1, columns);
	const float w = (avail - gap * (columns - 1)) / columns;
	bool changed = false;
	ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(gap, gap));
	for (int i = 0; i < (int)labels.size(); i++) {
		if (i % columns != 0)
			ImGui::SameLine();
		std::string lbl = labels[i] + "##" + std::to_string(i);
		if (pill_impl(lbl.c_str(), i == selected, ImVec2(w, px(height)), mono, px(10))) {
			if (selected != i)
				changed = true;
			selected = i;
		}
	}
	ImGui::PopStyleVar();
	ImGui::PopID();
	return changed;
}

bool segmented(const char *id, const std::vector<std::string> &labels, int &selected)
{
	ImGui::PushID(id);
	const float avail = ImGui::GetContentRegionAvail().x;
	const float h = px(52);
	ImVec2 p = ImGui::GetCursorScreenPos();
	ImDrawList *dl = ImGui::GetWindowDrawList();
	dl->AddRectFilled(p, ImVec2(p.x + avail, p.y + h), col::bg0, px(12));
	dl->AddRect(p, ImVec2(p.x + avail, p.y + h), col::line, px(12), 0, px(1));
	const float pad = px(4);
	const float w = (avail - pad * 2 - pad * (labels.size() - 1)) / labels.size();
	bool changed = false;
	for (int i = 0; i < (int)labels.size(); i++) {
		ImVec2 cp(p.x + pad + i * (w + pad), p.y + pad);
		ImGui::SetCursorScreenPos(cp);
		bool hov;
		std::string bid = "##seg" + std::to_string(i);
		if (hovered_and_clicked(bid.c_str(), ImVec2(w, h - pad * 2), true, &hov) && selected != i) {
			selected = i;
			changed = true;
		}
		bool sel = selected == i;
		if (sel)
			dl->AddRectFilled(cp, ImVec2(cp.x + w, cp.y + h - pad * 2), col::text, px(9));
		ImVec2 ts = measure(fonts.small_semibold, labels[i].c_str());
		dl->AddText(fonts.small_semibold, fonts.small_semibold->FontSize,
			    ImVec2(cp.x + (w - ts.x) / 2, cp.y + (h - pad * 2 - ts.y) / 2),
			    sel ? col::ink : (hov ? col::text : col::muted), labels[i].c_str());
	}
	ImGui::SetCursorScreenPos(ImVec2(p.x, p.y + h));
	ImGui::Dummy(ImVec2(avail, 0));
	ImGui::PopID();
	return changed;
}

bool toggle(const char *id, bool *value)
{
	ImVec2 p = ImGui::GetCursorScreenPos();
	ImVec2 size(px(52), px(44));
	bool hov;
	bool clicked = hovered_and_clicked(id, size, true, &hov);
	if (clicked)
		*value = !*value;
	ImDrawList *dl = ImGui::GetWindowDrawList();
	float tw = px(44), th = px(26);
	ImVec2 tp(p.x + (size.x - tw) / 2, p.y + (size.y - th) / 2);
	dl->AddRectFilled(tp, ImVec2(tp.x + tw, tp.y + th), *value ? col::accent : col::off, th / 2);
	float knob = px(20);
	float kx = *value ? tp.x + tw - knob - px(3) : tp.x + px(3);
	dl->AddCircleFilled(ImVec2(kx + knob / 2, tp.y + th / 2), knob / 2, *value ? col::ink : col::dim);
	return clicked;
}

bool toggle_row(const char *label, const char *desc, bool *value)
{
	ImGui::PushID(label);
	ImDrawList *dl = ImGui::GetWindowDrawList();
	const float avail = ImGui::GetContentRegionAvail().x;
	ImVec2 p = ImGui::GetCursorScreenPos();
	dl->AddLine(p, ImVec2(p.x + avail, p.y), col::line2, px(1));

	const float text_w = avail - px(64);
	ImVec2 ls = fonts.body_medium->CalcTextSizeA(fonts.body_medium->FontSize, FLT_MAX, text_w, label);
	ImVec2 ds = desc && *desc ? fonts.small->CalcTextSizeA(fonts.small->FontSize, FLT_MAX, text_w, desc) : ImVec2(0, 0);
	float content_h = ls.y + (ds.y > 0 ? ds.y + px(3) : 0);
	float row_h = std::max(px(56), content_h + px(16));

	float ty = p.y + (row_h - content_h) / 2;
	dl->AddText(fonts.body_medium, fonts.body_medium->FontSize, ImVec2(p.x, ty), col::text, label, nullptr, text_w);
	if (ds.y > 0)
		dl->AddText(fonts.small, fonts.small->FontSize, ImVec2(p.x, ty + ls.y + px(3)), col::muted, desc, nullptr,
			    text_w);

	ImGui::SetCursorScreenPos(ImVec2(p.x + avail - px(52), p.y + (row_h - px(44)) / 2));
	bool changed = toggle("##t", value);
	ImGui::SetCursorScreenPos(ImVec2(p.x, p.y + row_h));
	ImGui::Dummy(ImVec2(avail, 0));
	ImGui::PopID();
	return changed;
}

bool radio_card(const char *id, const char *title, const char *desc, bool selected, const char *tag, bool enabled)
{
	ImGui::PushID(id);
	const float avail = ImGui::GetContentRegionAvail().x;
	const float text_x = px(42);
	const float tag_w = tag && *tag ? measure(fonts.mono_small, tag).x + px(12) : 0;
	const float text_w = avail - text_x - px(14) - tag_w;
	ImVec2 ts = fonts.body_semibold->CalcTextSizeA(fonts.body_semibold->FontSize, FLT_MAX, text_w, title);
	ImVec2 ds = desc && *desc ? fonts.small->CalcTextSizeA(fonts.small->FontSize, FLT_MAX, text_w, desc) : ImVec2(0, 0);
	float h = std::max(px(52), ts.y + (ds.y > 0 ? ds.y + px(3) : 0) + px(24));

	ImVec2 p = ImGui::GetCursorScreenPos();
	bool hov;
	bool clicked = hovered_and_clicked("##card", ImVec2(avail, h), enabled, &hov);
	ImDrawList *dl = ImGui::GetWindowDrawList();
	ImVec2 q(p.x + avail, p.y + h);
	dl->AddRectFilled(p, q, selected ? col::bg3 : col::bg0, px(10));
	dl->AddRect(p, q, selected ? col::accent : (hov ? col::key_border : col::line), px(10), 0, px(1));
	float alpha = enabled ? 1.0f : 0.45f;
	auto a = [&](ImU32 c) { return (c & 0x00FFFFFF) | ((ImU32)(((c >> 24) & 0xFF) * alpha) << 24); };

	ImVec2 rc(p.x + px(22), p.y + px(12) + ts.y / 2 + px(1));
	dl->AddCircle(rc, px(7), a(selected ? col::accent : col::ring), 0, px(2));
	if (selected)
		dl->AddCircleFilled(rc, px(3), col::accent);

	float ty = p.y + px(12);
	dl->AddText(fonts.body_semibold, fonts.body_semibold->FontSize, ImVec2(p.x + text_x, ty), a(col::text), title,
		    nullptr, text_w);
	if (ds.y > 0)
		dl->AddText(fonts.small, fonts.small->FontSize, ImVec2(p.x + text_x, ty + ts.y + px(3)), a(col::muted), desc,
			    nullptr, text_w);
	if (tag_w > 0)
		dl->AddText(fonts.mono_small, fonts.mono_small->FontSize, ImVec2(q.x - tag_w, ty + px(2)),
			    a(col::accent_text), tag);
	ImGui::PopID();
	return clicked;
}

bool nav_item(const char *label, Icon icon, bool active, const char *badge)
{
	const float avail = ImGui::GetContentRegionAvail().x;
	ImVec2 p = ImGui::GetCursorScreenPos();
	bool hov;
	bool clicked = hovered_and_clicked(label, ImVec2(avail, px(44)), true, &hov);
	ImDrawList *dl = ImGui::GetWindowDrawList();
	if (active || hov)
		dl->AddRectFilled(p, ImVec2(p.x + avail, p.y + px(44)), active ? col::bg3 : col::bg2, px(8));
	draw_icon(dl, icon, ImVec2(p.x + px(21), p.y + px(22)), px(18), active ? col::accent : (hov ? col::text : col::muted));
	ImFont *f = active ? fonts.body_semibold : fonts.body_medium;
	const char *end = ImGui::FindRenderedTextEnd(label);
	dl->AddText(f, f->FontSize, ImVec2(p.x + px(42), p.y + (px(44) - f->FontSize) / 2),
		    active || hov ? col::text : col::muted, label, end);
	if (badge && *badge) {
		ImVec2 bs = measure(fonts.mono_small, badge);
		dl->AddText(fonts.mono_small, fonts.mono_small->FontSize,
			    ImVec2(p.x + avail - bs.x - px(12), p.y + (px(44) - bs.y) / 2), col::muted, badge);
	}
	return clicked;
}

ImVec2 keycap(ImDrawList *dl, ImVec2 pos, const char *s, bool accent)
{
	ImVec2 ts = measure(fonts.mono_small, s);
	ImVec2 size(ts.x + px(16), ts.y + px(10));
	ImVec2 q(pos.x + size.x, pos.y + size.y);
	if (accent) {
		dl->AddRectFilled(pos, q, col::accent, px(5));
		dl->AddText(fonts.mono_bold, fonts.mono_small->FontSize, ImVec2(pos.x + px(8), pos.y + px(5)), col::ink, s);
	} else {
		dl->AddRectFilled(pos, q, col::bg3, px(5));
		dl->AddRect(pos, q, col::key_border, px(5), 0, px(1));
		dl->AddText(fonts.mono_small, fonts.mono_small->FontSize, ImVec2(pos.x + px(8), pos.y + px(5)), col::text, s);
	}
	return size;
}

bool hotkey_button(const char *id, const hl::Hotkey &key, bool listening, bool accent)
{
	std::vector<std::string> parts;
	if (!listening) {
		if (key.empty()) {
			parts.push_back("Not set");
		} else {
			if (key.ctrl)
				parts.push_back("Ctrl");
			if (key.shift)
				parts.push_back("Shift");
			if (key.alt)
				parts.push_back("Alt");
			if (key.win)
				parts.push_back("Win");
			parts.push_back(hl::key_name(key.vk));
		}
	}

	ImDrawList *dl = ImGui::GetWindowDrawList();
	float plus_w = measure(fonts.small, "+").x + px(8);
	float w = px(12);
	if (listening) {
		w += measure(fonts.small_semibold, "Press a key…").x + px(8);
	} else {
		for (size_t i = 0; i < parts.size(); i++)
			w += measure(fonts.mono_small, parts[i].c_str()).x + px(16) + (i ? plus_w : 0);
	}
	ImVec2 size(std::max(w, px(56)), px(40));
	ImVec2 p = ImGui::GetCursorScreenPos();
	bool hov;
	bool clicked = hovered_and_clicked(id, size, true, &hov);
	ImVec2 q(p.x + size.x, p.y + size.y);
	dl->AddRectFilled(p, q, col::bg0, px(8));
	dl->AddRect(p, q, listening ? col::accent : (hov ? col::key_border : col::line), px(8), 0,
		    px(listening ? 1.5f : 1.0f));

	if (listening) {
		float ts_h = fonts.small_semibold->FontSize;
		dl->AddText(fonts.small_semibold, ts_h, ImVec2(p.x + px(10), p.y + (size.y - ts_h) / 2), col::accent_text,
			    "Press a key…");
	} else {
		float x = p.x + px(6);
		for (size_t i = 0; i < parts.size(); i++) {
			if (i) {
				dl->AddText(fonts.small, fonts.small->FontSize, ImVec2(x + px(2), p.y + px(11)), col::dim, "+");
				x += plus_w;
			}
			bool is_main = i + 1 == parts.size();
			ImVec2 ks = keycap(dl, ImVec2(x, p.y + (size.y - px(22)) / 2), parts[i].c_str(),
					   accent && is_main && !key.empty());
			x += ks.x;
		}
	}
	return clicked;
}

/* ------------------------------------------------------------------ */
/* cards                                                              */

void begin_card(const char *id, float width, bool dashed)
{
	ImGui::PushStyleColor(ImGuiCol_ChildBg, dashed ? col::bg1 : col::bg2);
	ImGui::PushStyleColor(ImGuiCol_Border, dashed ? col::key_border : col::line);
	ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, px(14));
	ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(px(20), px(20)));
	ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(px(10), px(12)));
	ImGui::BeginChild(id, ImVec2(width, 0),
			  ImGuiChildFlags_Borders | ImGuiChildFlags_AutoResizeY | ImGuiChildFlags_AlwaysUseWindowPadding,
			  ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
}

void end_card()
{
	ImGui::EndChild();
	ImGui::PopStyleVar(3);
	ImGui::PopStyleColor(2);
}

void card_header(const char *title, const char *desc)
{
	text(fonts.h2, col::text, "%s", title);
	if (desc && *desc) {
		ImGui::SetCursorPosY(ImGui::GetCursorPosY() - px(8));
		text_wrapped(fonts.small, col::muted, ImGui::GetContentRegionAvail().x, "%s", desc);
	}
	ImGui::Dummy(ImVec2(0, px(2)));
}

/* ------------------------------------------------------------------ */
/* inputs                                                             */

static void push_field_style()
{
	ImGui::PushStyleColor(ImGuiCol_FrameBg, col::bg0);
	ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, col::bg0);
	ImGui::PushStyleColor(ImGuiCol_Border, col::line);
	ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, px(1));
	ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, px(10));
	ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(px(12), px(13)));
}

static void pop_field_style()
{
	ImGui::PopStyleVar(3);
	ImGui::PopStyleColor(3);
}

bool combo(const char *label, int *current, const std::vector<std::string> &items, float width)
{
	ImGui::BeginGroup();
	if (label && *label && label[0] != '#') {
		text(fonts.small_semibold, col::text, "%s", label);
		ImGui::SetCursorPosY(ImGui::GetCursorPosY() - px(4));
	}
	push_field_style();
	ImGui::SetNextItemWidth(width);
	std::string id = std::string("##") + label;
	const char *preview = (*current >= 0 && *current < (int)items.size()) ? items[*current].c_str() : "";
	bool changed = false;
	ImGui::PushFont(fonts.body);
	if (ImGui::BeginCombo(id.c_str(), preview)) {
		for (int i = 0; i < (int)items.size(); i++) {
			bool sel = i == *current;
			if (ImGui::Selectable(items[i].c_str(), sel, 0, ImVec2(0, px(30)))) {
				changed = *current != i;
				*current = i;
			}
			if (sel)
				ImGui::SetItemDefaultFocus();
		}
		ImGui::EndCombo();
	}
	ImGui::PopFont();
	pop_field_style();
	ImGui::EndGroup();
	return changed;
}

bool slider_int(const char *id, int *v, int lo, int hi, const char *fmt, float width)
{
	push_field_style();
	ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(px(12), px(8)));
	ImGui::SetNextItemWidth(width);
	ImGui::PushFont(fonts.mono);
	bool changed = ImGui::SliderInt(id, v, lo, hi, fmt, ImGuiSliderFlags_AlwaysClamp);
	ImGui::PopFont();
	ImGui::PopStyleVar();
	pop_field_style();
	return changed;
}

bool input_text(const char *id, std::string &value, float width, const char *hint)
{
	char buf[1024];
	snprintf(buf, sizeof(buf), "%s", value.c_str());
	push_field_style();
	ImGui::SetNextItemWidth(width);
	ImGui::PushFont(fonts.mono);
	bool changed = hint ? ImGui::InputTextWithHint(id, hint, buf, sizeof(buf)) : ImGui::InputText(id, buf, sizeof(buf));
	ImGui::PopFont();
	pop_field_style();
	if (changed)
		value = buf;
	return changed;
}

void value_box(const char *s, float width, bool mono, ImU32 color)
{
	ImVec2 p = ImGui::GetCursorScreenPos();
	ImVec2 size(width, px(44));
	ImDrawList *dl = ImGui::GetWindowDrawList();
	dl->AddRectFilled(p, ImVec2(p.x + size.x, p.y + size.y), col::bg0, px(10));
	dl->AddRect(p, ImVec2(p.x + size.x, p.y + size.y), col::line, px(10), 0, px(1));
	ImFont *f = mono ? fonts.mono : fonts.body;
	text_ellipsis(dl, f, ImVec2(p.x + px(12), p.y + (size.y - f->FontSize) / 2), size.x - px(24), color, s);
	ImGui::Dummy(size);
}

void status_dot(ImDrawList *dl, ImVec2 center, float r, ImU32 color)
{
	ImU32 ring_col = (color & 0x00FFFFFF) | (0x33u << 24);
	dl->AddCircleFilled(center, r + px(3), ring_col);
	dl->AddCircleFilled(center, r, color);
}

} // namespace ui

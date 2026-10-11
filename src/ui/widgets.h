#pragma once

/* Small custom widgets that give the ImGui UI the look of the design:
 * pills, switches, radio cards, keycaps, cards and line icons. */

#include "theme.h"

#include "../common/settings.h"

#include <string>
#include <vector>

namespace ui {

enum class Icon {
	Grid,
	Scissors,
	Monitor,
	Speaker,
	SpeakerMute,
	Bell,
	Folder,
	Play,
	Pause,
	Star,
	StarFilled,
	More,
	Search,
	Trash,
	Pencil,
	ChevronLeft,
	Minus,
	Square,
	Close,
	Plus,
	Camera,
	Mic,
	Check,
	Share,
	Diamond,
	FrameBack,
	FrameForward,
	Record,
	Logo,
};

void draw_icon(ImDrawList *dl, Icon icon, ImVec2 center, float size, ImU32 color);
/* The app icon followed by the "highlight" wordmark (pos: the icon's top-left corner).
 * Returns the x where the wordmark ends. */
float brand_lockup(ImDrawList *dl, ImVec2 pos, float icon_size, ImFont *font);

/* text */
void text(ImFont *font, ImU32 color, const char *fmt, ...);
void text_wrapped(ImFont *font, ImU32 color, float wrap_width, const char *fmt, ...);
ImVec2 measure(ImFont *font, const char *s);
/* Draws text clipped with an ellipsis into [pos, pos + max_w]. */
void text_ellipsis(ImDrawList *dl, ImFont *font, ImVec2 pos, float max_w, ImU32 color, const std::string &s);

void section_label(const char *label);

/* buttons */
bool primary_button(const char *label, ImVec2 size, const char *key = nullptr, bool enabled = true);
bool secondary_button(const char *label, ImVec2 size, Icon icon = Icon::Logo, bool enabled = true);
bool icon_button(const char *id, Icon icon, float size, const char *tooltip = nullptr, ImU32 color = col::muted,
		 bool framed = false);
bool pill(const char *label, bool selected, ImVec2 size, bool mono = false);
/* A grid of pills. Returns true when the selection changed. */
bool pill_grid(const char *id, const std::vector<std::string> &labels, int &selected, int columns, bool mono = false,
	       float height = 44.0f);
bool segmented(const char *id, const std::vector<std::string> &labels, int &selected);

bool toggle(const char *id, bool *value);
/* Label + description on the left, switch on the right, separator on top. */
bool toggle_row(const char *label, const char *desc, bool *value);
bool radio_card(const char *id, const char *title, const char *desc, bool selected, const char *tag = nullptr,
		bool enabled = true);
bool nav_item(const char *label, Icon icon, bool active, const char *badge = nullptr);

/* A keycap chip (non-interactive). Returns its size. */
ImVec2 keycap(ImDrawList *dl, ImVec2 pos, const char *text, bool accent = false);
/* "Ctrl + Shift + F8" as keycaps in a framed button. */
bool hotkey_button(const char *id, const hl::Hotkey &key, bool listening, bool accent = false);

/* cards */
void begin_card(const char *id, float width, bool dashed = false);
void end_card();
void card_header(const char *title, const char *desc = nullptr);

/* inputs */
bool combo(const char *label, int *current, const std::vector<std::string> &items, float width);
bool slider_int(const char *id, int *v, int lo, int hi, const char *fmt, float width);
bool input_text(const char *id, std::string &value, float width, const char *hint = nullptr);
void value_box(const char *text, float width, bool mono = true, ImU32 color = col::text);

/* status dot with a soft ring */
void status_dot(ImDrawList *dl, ImVec2 center, float r, ImU32 color);

} // namespace ui

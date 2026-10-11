#pragma once

#include <imgui.h>

namespace ui {

/* Brand colors (see the design's Brand board). */
namespace col {
constexpr ImU32 bg0 = IM_COL32(0x0B, 0x0C, 0x0E, 255);   /* window */
constexpr ImU32 bg1 = IM_COL32(0x11, 0x13, 0x17, 255);   /* sidebar, panels */
constexpr ImU32 bg2 = IM_COL32(0x16, 0x18, 0x1D, 255);   /* cards */
constexpr ImU32 bg3 = IM_COL32(0x1D, 0x20, 0x26, 255);   /* raised */
constexpr ImU32 line = IM_COL32(0x26, 0x2A, 0x31, 255);
constexpr ImU32 line2 = IM_COL32(0x1F, 0x23, 0x29, 255);
constexpr ImU32 key_border = IM_COL32(0x33, 0x38, 0x42, 255);
constexpr ImU32 text = IM_COL32(0xEC, 0xED, 0xEF, 255);
constexpr ImU32 muted = IM_COL32(0xA0, 0xA6, 0xB0, 255);
constexpr ImU32 dim = IM_COL32(0x85, 0x8B, 0x96, 255);
constexpr ImU32 accent = IM_COL32(0xFF, 0x5A, 0x1F, 255);
constexpr ImU32 accent_text = IM_COL32(0xFF, 0x7A, 0x45, 255);
constexpr ImU32 accent_dim = IM_COL32(0x7A, 0x3A, 0x22, 255);
constexpr ImU32 ink = IM_COL32(0x0B, 0x0C, 0x0E, 255);
constexpr ImU32 off = IM_COL32(0x2E, 0x33, 0x3B, 255);
constexpr ImU32 ring = IM_COL32(0x4A, 0x50, 0x5B, 255);
constexpr ImU32 wave = IM_COL32(0xC9, 0xCD, 0xD3, 255);
constexpr ImU32 wave_dim = IM_COL32(0x6B, 0x71, 0x7C, 255);
constexpr ImU32 shade = IM_COL32(0x0B, 0x0C, 0x0E, 184);
} // namespace col

struct Fonts {
	ImFont *body = nullptr;          /* Archivo 14 */
	ImFont *body_medium = nullptr;   /* Archivo Medium 14 */
	ImFont *body_semibold = nullptr; /* Archivo SemiBold 14 */
	ImFont *body_bold = nullptr;     /* Archivo Bold 15 */
	ImFont *caption = nullptr;         /* Archivo 12.5 */
	ImFont *small_semibold = nullptr;
	ImFont *h1 = nullptr;       /* Archivo Expanded ExtraBold 30 */
	ImFont *h2 = nullptr;       /* Archivo Bold 16 */
	ImFont *title = nullptr;    /* Archivo Expanded ExtraBold 18 */
	ImFont *wordmark = nullptr; /* Archivo Expanded ExtraBold 13 */
	ImFont *big = nullptr;      /* Archivo Expanded ExtraBold 38 */
	ImFont *mono = nullptr;     /* JetBrains Mono Medium 13 */
	ImFont *mono_small = nullptr;
	ImFont *mono_bold = nullptr;
};

extern Fonts fonts;
extern float dpi_scale;

/* Builds the font atlas for the given DPI scale and applies the style. */
void setup_theme(float scale);

inline float px(float v)
{
	return v * dpi_scale;
}

} // namespace ui

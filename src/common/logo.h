#pragma once

#include <vector>

namespace hl {

/* The Highlight mark: two slanted bars forming an H (the right one notched) around a
 * play triangle. Filled outlines with rounded corners, clockwise (screen coordinates,
 * y down), on a k_logo_w x k_logo_h unit grid. The same shapes draw the app icon, the
 * tray icon, the banner icon and the window logo; scripts/make-brand.py renders the
 * image files from the same numbers. */
struct LogoPoint {
	float x, y;
};

constexpr float k_logo_w = 96.0f;
constexpr float k_logo_h = 110.0f;

/* left bar, right bar, triangle */
const std::vector<std::vector<LogoPoint>> &logo_mark();

/* Height of the mark relative to the rounded square it sits on; a bit larger at icon
 * sizes so it stays readable. */
inline float logo_mark_fraction(float tile_px)
{
	return tile_px <= 24.0f ? 0.68f : 0.62f;
}

/* Where the mark goes on an icon tile tile_px pixels wide (relative to the tile's
 * top-left corner): mark point (x, y) of outline i lands at
 * (x * scale + this->x + dx[i], y * scale + this->y). Tiles up to 48 px are hinted so the
 * bars' upright edges sit on pixel boundaries and small icons stay sharp. */
struct LogoPlacement {
	float scale = 1, x = 0, y = 0;
	float dx[3] = {0, 0, 0}; /* left bar, right bar, triangle */
};
LogoPlacement logo_icon_placement(float tile_px);

} // namespace hl

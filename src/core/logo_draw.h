#pragma once

/* GDI+ drawing of the Highlight mark (src/common/logo.h). */

#include "gdiplus_include.h"
#include "../common/logo.h"

#include <vector>

namespace hl {

/* Fills the mark centred on (cx, cy), `height` pixels tall. */
inline void fill_logo_mark(Gdiplus::Graphics &g, const Gdiplus::Brush &brush, float cx, float cy, float height)
{
	const float s = height / k_logo_h;
	const float ox = cx - k_logo_w * s / 2, oy = cy - height / 2;
	std::vector<Gdiplus::PointF> pts;
	for (const auto &poly : logo_mark()) {
		pts.clear();
		for (const LogoPoint &p : poly)
			pts.emplace_back(ox + p.x * s, oy + p.y * s);
		g.FillPolygon(&brush, pts.data(), (INT)pts.size());
	}
}

} // namespace hl

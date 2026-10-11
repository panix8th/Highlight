#include "icons.h"
#include "gdiplus_include.h"
#include "logo_draw.h"

namespace hl {

using namespace Gdiplus;

static void rounded_rect(GraphicsPath &path, REAL x, REAL y, REAL w, REAL h, REAL r)
{
	REAL d = r * 2;
	path.AddArc(x, y, d, d, 180, 90);
	path.AddArc(x + w - d, y, d, d, 270, 90);
	path.AddArc(x + w - d, y + h - d, d, d, 0, 90);
	path.AddArc(x, y + h - d, d, d, 90, 90);
	path.CloseFigure();
}

HICON make_icon(IconState state, int size)
{
	Bitmap bmp(size, size, PixelFormat32bppARGB);
	Graphics g(&bmp);
	g.SetSmoothingMode(SmoothingModeAntiAlias);
	g.SetPixelOffsetMode(PixelOffsetModeHighQuality);
	g.Clear(Color(0, 0, 0, 0));

	const bool lit = state == IconState::Live || state == IconState::Saved || state == IconState::Recording;
	const Color bg = lit ? Color(255, 0xFF, 0x5A, 0x1F) : Color(255, 0x3A, 0x3F, 0x48);
	const Color fg = lit ? Color(255, 0x0B, 0x0C, 0x0E) : Color(255, 0xC9, 0xCD, 0xD3);

	/* The tile and the status glyphs are drawn on a 40-unit grid. */
	const REAL s = (REAL)size / 40.0f;
	GraphicsPath box;
	rounded_rect(box, 0, 0, (REAL)size, (REAL)size, 10 * s);
	SolidBrush bg_brush(bg);
	g.FillPath(&bg_brush, &box);

	SolidBrush fg_brush(fg);
	if (state == IconState::Saved) {
		Pen pen(fg, 4.5f * s);
		pen.SetStartCap(LineCapRound);
		pen.SetEndCap(LineCapRound);
		pen.SetLineJoin(LineJoinRound);
		PointF pts[] = {PointF(11 * s, 20.5f * s), PointF(17 * s, 26.5f * s), PointF(29 * s, 13.5f * s)};
		g.DrawLines(&pen, pts, 3);
	} else if (state == IconState::Paused) {
		g.FillRectangle(&fg_brush, 12 * s, 11 * s, 5.5f * s, 18 * s);
		g.FillRectangle(&fg_brush, 22.5f * s, 11 * s, 5.5f * s, 18 * s);
	} else {
		fill_logo_mark(g, fg_brush, size / 2.0f, size / 2.0f, size * logo_mark_fraction((float)size));
		if (state == IconState::Recording) {
			/* a ring in the tile colour keeps the dot apart from the mark's right bar */
			Region tile_area(&box);
			g.SetClip(&tile_area);
			g.FillEllipse(&bg_brush, 26.5f * s, -1.0f * s, 14 * s, 14 * s);
			g.ResetClip();
			SolidBrush dot(Color(255, 0xEC, 0xED, 0xEF));
			g.FillEllipse(&dot, 28 * s, 0.5f * s, 11 * s, 11 * s);
		}
	}

	HICON icon = nullptr;
	bmp.GetHICON(&icon);
	return icon;
}

} // namespace hl

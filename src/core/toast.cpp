#include "toast.h"
#include "gdiplus_include.h"
#include "logo_draw.h"

#include "../common/paths.h"
#include "../common/strings.h"

#include <chrono>
#include <algorithm>
#include <cmath>
#include <functional>
#include <vector>

namespace hl {

using namespace Gdiplus;

static const wchar_t *k_class = L"HighlightOverlay";
static constexpr UINT_PTR k_anim_timer = 1;

static const Color k_text(255, 0xEC, 0xED, 0xEF);
static const Color k_muted(255, 0xA0, 0xA6, 0xB0);
static const Color k_orange(255, 0xFF, 0x5A, 0x1F);
static const Color k_ink(255, 0x0B, 0x0C, 0x0E);

static double now_s()
{
	using namespace std::chrono;
	return duration<double>(steady_clock::now().time_since_epoch()).count();
}

static float ui_scale()
{
	return (float)GetDpiForSystem() / 96.0f;
}

static float ease_out(float t)
{
	t = std::clamp(t, 0.0f, 1.0f);
	return 1.0f - (1.0f - t) * (1.0f - t) * (1.0f - t);
}

static void rounded(GraphicsPath &p, REAL x, REAL y, REAL w, REAL h, REAL r)
{
	REAL d = r * 2;
	p.AddArc(x, y, d, d, 180, 90);
	p.AddArc(x + w - d, y, d, d, 270, 90);
	p.AddArc(x + w - d, y + h - d, d, d, 0, 90);
	p.AddArc(x, y + h - d, d, d, 90, 90);
	p.CloseFigure();
}

static void update_layered(HWND hwnd, int x, int y, int w, int h, BYTE alpha,
			   const std::function<void(Graphics &)> &draw)
{
	BITMAPINFO bi = {};
	bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
	bi.bmiHeader.biWidth = w;
	bi.bmiHeader.biHeight = -h;
	bi.bmiHeader.biPlanes = 1;
	bi.bmiHeader.biBitCount = 32;
	bi.bmiHeader.biCompression = BI_RGB;

	HDC screen = GetDC(nullptr);
	HDC mem = CreateCompatibleDC(screen);
	void *bits = nullptr;
	HBITMAP dib = CreateDIBSection(screen, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
	if (!dib) {
		DeleteDC(mem);
		ReleaseDC(nullptr, screen);
		return;
	}
	HGDIOBJ old = SelectObject(mem, dib);
	{
		Bitmap bmp(w, h, w * 4, PixelFormat32bppPARGB, (BYTE *)bits);
		Graphics g(&bmp);
		g.SetSmoothingMode(SmoothingModeAntiAlias);
		g.SetPixelOffsetMode(PixelOffsetModeHighQuality);
		g.SetTextRenderingHint(TextRenderingHintAntiAlias);
		g.Clear(Color(0, 0, 0, 0));
		draw(g);
	}
	POINT dst = {x, y};
	SIZE size = {w, h};
	POINT src = {0, 0};
	BLENDFUNCTION bf = {AC_SRC_OVER, 0, alpha, AC_SRC_ALPHA};
	UpdateLayeredWindow(hwnd, screen, &dst, &size, mem, &src, 0, &bf, ULW_ALPHA);

	SelectObject(mem, old);
	DeleteObject(dib);
	DeleteDC(mem);
	ReleaseDC(nullptr, screen);
}

static RECT primary_monitor()
{
	HMONITOR mon = MonitorFromPoint(POINT{0, 0}, MONITOR_DEFAULTTOPRIMARY);
	MONITORINFO mi = {sizeof(mi)};
	GetMonitorInfoW(mon, &mi);
	return mi.rcMonitor;
}

/* The card itself, drawn at scale s into a w x h ARGB surface. Kept separate
 * from the window code so the same drawing can be rendered off screen. */
static void draw_card(Graphics &g, int w, int h, float s, const ToastContent &c, float bar_from, float bar_to,
		      double now, PrivateFontCollection *pfc, const std::wstring &title_family,
		      const std::wstring &mono_family)
{
	GraphicsPath bg;
	rounded(bg, 0.5f, 0.5f, (REAL)w - 1, (REAL)h - 1, 14 * s);
	SolidBrush bg_brush(Color(250, 0x11, 0x13, 0x17));
	g.FillPath(&bg_brush, &bg);
	Pen border(Color(255, 0x2A, 0x2E, 0x36), 1.0f * s);
	g.DrawPath(&border, &bg);

	/* bar along the bottom edge, clipped to the card */
	if (bar_to > bar_from) {
		Region clip(&bg);
		g.SetClip(&clip);
		SolidBrush track(Color(255, 0x1D, 0x20, 0x26));
		g.FillRectangle(&track, 0.0f, (REAL)h - 4 * s, (REAL)w, 4 * s);
		SolidBrush fill(k_orange);
		g.FillRectangle(&fill, w * bar_from, (REAL)h - 4 * s, w * (bar_to - bar_from), 4 * s);
		g.ResetClip();
	}

	/* icon */
	GraphicsPath box;
	rounded(box, 16 * s, 18 * s, 42 * s, 42 * s, 10 * s);
	const bool accent = c.kind != ToastKind::Error;
	SolidBrush box_brush(accent ? k_orange : Color(255, 0x3A, 0x3F, 0x48));
	g.FillPath(&box_brush, &box);
	Pen glyph(accent ? k_ink : k_text, 3.0f * s);
	glyph.SetStartCap(LineCapRound);
	glyph.SetEndCap(LineCapRound);
	glyph.SetLineJoin(LineJoinRound);
	SolidBrush ink(accent ? k_ink : k_text);
	const REAL cx = 37 * s, cy = 39 * s;
	switch (c.kind) {
	case ToastKind::Saved: {
		PointF pts[] = {PointF(cx - 10 * s, cy + 0.5f * s), PointF(cx - 3.5f * s, cy + 7 * s),
				PointF(cx + 10 * s, cy - 7 * s)};
		g.DrawLines(&glyph, pts, 3);
		break;
	}
	case ToastKind::Bookmark: {
		PointF pts[] = {PointF(cx, cy - 9 * s), PointF(cx + 9 * s, cy), PointF(cx, cy + 9 * s),
				PointF(cx - 9 * s, cy)};
		g.FillPolygon(&ink, pts, 4);
		break;
	}
	case ToastKind::Error:
		g.DrawLine(&glyph, cx, cy - 10 * s, cx, cy + 2 * s);
		g.FillEllipse(&ink, cx - 1.8f * s, cy + 6 * s, 3.6f * s, 3.6f * s);
		break;
	case ToastKind::Working: {
		/* spinning arc */
		float start = (float)std::fmod(now * 400.0, 360.0);
		g.DrawArc(&glyph, cx - 9 * s, cy - 9 * s, 18 * s, 18 * s, start, 270.0f);
		break;
	}
	case ToastKind::Info:
	default: {
		/* the Highlight mark, as on the app icon */
		fill_logo_mark(g, ink, cx, cy, 42 * s * logo_mark_fraction(42 * s));
		break;
	}
	}

	/* The expanded face is already ExtraBold; asking GDI+ for bold on top of it
	 * synthesizes an even heavier weight that fills in the counters. */
	const INT title_style = title_family.find(L"Expanded") != std::wstring::npos ? FontStyleRegular : FontStyleBold;
	Font title(title_family.c_str(), 15 * s, title_style, UnitPixel, pfc);
	Font mono(mono_family.c_str(), 12.5f * s, FontStyleRegular, UnitPixel, pfc);
	Font mono_small(mono_family.c_str(), 11 * s, FontStyleRegular, UnitPixel, pfc);
	SolidBrush text(k_text);
	SolidBrush muted(k_muted);

	StringFormat one_line;
	one_line.SetFormatFlags(StringFormatFlagsNoWrap);
	one_line.SetTrimming(StringTrimmingEllipsisCharacter);

	REAL right = (REAL)w - 16 * s;
	if (!c.key.empty()) {
		std::wstring key = widen(c.key);
		RectF kb;
		g.MeasureString(key.c_str(), -1, &mono_small, PointF(0, 0), &kb);
		REAL kw = kb.Width + 10 * s, kh = 20 * s;
		GraphicsPath chip;
		rounded(chip, right - kw, 15 * s, kw, kh, 5 * s);
		SolidBrush chip_bg(Color(255, 0x1D, 0x20, 0x26));
		Pen chip_border(Color(255, 0x33, 0x38, 0x42), 1.0f * s);
		g.FillPath(&chip_bg, &chip);
		g.DrawPath(&chip_border, &chip);
		g.DrawString(key.c_str(), -1, &mono_small, PointF(right - kw + 5 * s, 17.5f * s), &muted);
		right -= kw + 8 * s;
	}

	const REAL tx = 72 * s;
	std::wstring t = widen(c.title), l1 = widen(c.line1), l2 = widen(c.line2);
	g.DrawString(t.c_str(), -1, &title, RectF(tx, 14 * s, right - tx, 22 * s), &one_line, &text);
	g.DrawString(l1.c_str(), -1, &mono, RectF(tx, 40 * s, (REAL)w - tx - 16 * s, 20 * s), &one_line, &text);
	StringFormat path_fmt;
	path_fmt.SetFormatFlags(StringFormatFlagsNoWrap);
	path_fmt.SetTrimming(StringTrimmingEllipsisPath);
	g.DrawString(l2.c_str(), -1, &mono_small, RectF(tx, 62 * s, (REAL)w - tx - 16 * s, 18 * s), &path_fmt,
		     &muted);
}

/* ------------------------------------------------------------------ */

LRESULT CALLBACK Overlay::wndproc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
	if (msg == WM_NCCREATE) {
		auto *cs = (CREATESTRUCTW *)lp;
		SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR)cs->lpCreateParams);
	}
	if (msg == WM_TIMER && wp == k_anim_timer) {
		auto *self = (Overlay *)GetWindowLongPtrW(hwnd, GWLP_USERDATA);
		if (self && hwnd == self->toast_)
			self->tick();
		return 0;
	}
	if (msg == WM_MOUSEACTIVATE)
		return MA_NOACTIVATE;
	return DefWindowProcW(hwnd, msg, wp, lp);
}

bool Overlay::init(HINSTANCE inst)
{
	WNDCLASSEXW wc = {sizeof(wc)};
	wc.lpfnWndProc = wndproc;
	wc.hInstance = inst;
	wc.lpszClassName = k_class;
	RegisterClassExW(&wc);

	const DWORD ex = WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE;
	toast_ = CreateWindowExW(ex, k_class, L"Highlight", WS_POPUP, 0, 0, 1, 1, nullptr, nullptr, inst, this);
	rec_ = CreateWindowExW(ex, k_class, L"Highlight REC", WS_POPUP, 0, 0, 1, 1, nullptr, nullptr, inst, this);
	load_fonts();
	return toast_ && rec_;
}

void Overlay::shutdown()
{
	if (toast_) {
		KillTimer(toast_, k_anim_timer);
		DestroyWindow(toast_);
	}
	if (rec_)
		DestroyWindow(rec_);
	toast_ = rec_ = nullptr;
	delete (PrivateFontCollection *)fonts_;
	fonts_ = nullptr;
}

void Overlay::load_fonts()
{
	auto *pfc = new PrivateFontCollection();
	fonts_ = pfc;
	auto dir = paths::fonts_dir();
	const wchar_t *files[] = {L"Archivo-Bold.ttf", L"Archivo-Medium.ttf", L"ArchivoExpanded-ExtraBold.ttf",
				  L"JetBrainsMono-Medium.ttf"};
	for (const wchar_t *f : files)
		pfc->AddFontFile((dir / f).c_str());

	int count = pfc->GetFamilyCount();
	std::vector<FontFamily> fams(count > 0 ? count : 0);
	int found = 0;
	if (count > 0)
		pfc->GetFamilies(count, fams.data(), &found);
	for (int i = 0; i < found; i++) {
		WCHAR name[LF_FACESIZE] = {};
		fams[i].GetFamilyName(name);
		std::wstring n = name;
		if (n.find(L"Mono") != std::wstring::npos)
			mono_family_ = n;
		else if (n.find(L"Expanded") != std::wstring::npos)
			title_family_ = n;
		else if (n.find(L"Archivo") != std::wstring::npos)
			body_family_ = n;
	}
	if (body_family_.empty())
		body_family_ = L"Segoe UI";
	if (title_family_.empty())
		title_family_ = body_family_;
	if (mono_family_.empty())
		mono_family_ = L"Consolas";
}

void Overlay::begin(const ToastContent &c, const std::string &position)
{
	const double now = now_s();
	content_ = c;
	position_ = position;
	/* Replace the content in place when already on screen; slide in otherwise. */
	if (!visible_ || hiding_since_ > 0) {
		shown_at_ = now;
		hiding_since_ = 0;
	}
	visible_ = true;
	SetWindowPos(toast_, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_SHOWWINDOW);
	tick();
	SetTimer(toast_, k_anim_timer, 16, nullptr);
}

void Overlay::show_toast(const ToastContent &c, const std::string &position, int seconds)
{
	if (!toast_)
		return;
	display_secs_ = std::max(1, seconds);
	hide_at_ = now_s() + display_secs_;
	progress_secs_ = 0;
	begin(c, position);
}

void Overlay::show_progress(const ToastContent &c, const std::string &position, double expected_seconds)
{
	if (!toast_)
		return;
	ToastContent w = c;
	w.kind = ToastKind::Working;
	progress_start_ = now_s();
	progress_secs_ = std::max(0.5, expected_seconds);
	hide_at_ = 0; /* stays until the result replaces it */
	begin(w, position);
}

void Overlay::tick()
{
	if (!visible_) {
		KillTimer(toast_, k_anim_timer);
		return;
	}
	const double now = now_s();
	/* Safety net: a working card whose result never arrives must not stay forever. */
	if (content_.kind == ToastKind::Working && hide_at_ == 0 && now - progress_start_ > progress_secs_ + 90)
		hide_at_ = now;
	if (hide_at_ > 0 && now >= hide_at_ && hiding_since_ == 0)
		hiding_since_ = now;

	float in = ease_out((float)((now - shown_at_) / 0.24));
	float out = hiding_since_ > 0 ? ease_out((float)((now - hiding_since_) / 0.22)) : 0.0f;
	if (out >= 1.0f) {
		visible_ = false;
		KillTimer(toast_, k_anim_timer);
		ShowWindow(toast_, SW_HIDE);
		return;
	}
	paint(in * (1.0f - out));
}

void Overlay::paint(float visibility)
{
	const float s = ui_scale();
	const int w = (int)(392 * s), h = (int)(96 * s);
	const int margin = (int)(24 * s);
	RECT mon = primary_monitor();

	const bool left = position_ == "top-left" || position_ == "bottom-left";
	int x = left ? mon.left + margin : mon.right - w - margin;
	int y = mon.top + margin;
	if (position_ == "bottom-left" || position_ == "bottom-right")
		y = mon.bottom - h - margin * 3;
	/* slide in from the nearest screen edge */
	const int slide = (int)((1.0f - visibility) * (w * 0.4f));
	x += left ? -slide : slide;

	const double now = now_s();
	auto *pfc = (PrivateFontCollection *)fonts_;
	const ToastContent c = content_;

	/* bar: progress while working, remaining display time when done */
	float bar_from = 0, bar_to = 0;
	if (c.kind == ToastKind::Working) {
		double t = (now - progress_start_) / progress_secs_;
		if (t < 1.0) {
			bar_to = (float)(t * 0.92);
		} else {
			/* still writing the file: a moving segment */
			float phase = (float)std::fmod(now * 0.9, 1.0);
			bar_from = std::max(0.0f, phase * 1.3f - 0.3f);
			bar_to = std::min(1.0f, phase * 1.3f);
		}
	} else if (hide_at_ > 0) {
		bar_to = (float)std::clamp((hide_at_ - now) / display_secs_, 0.0, 1.0);
	}

	update_layered(toast_, x, y, w, h, (BYTE)(255 * visibility), [&](Graphics &g) {
		draw_card(g, w, h, s, c, bar_from, bar_to, now, pfc, title_family_, mono_family_);
	});
}

void Overlay::set_recording(bool on, int64_t seconds)
{
	if (!rec_)
		return;
	if (!on) {
		ShowWindow(rec_, SW_HIDE);
		return;
	}
	paint_rec(seconds);
	SetWindowPos(rec_, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_SHOWWINDOW);
}

void Overlay::paint_rec(int64_t seconds)
{
	const float s = ui_scale();
	const int w = (int)(118 * s), h = (int)(28 * s);
	RECT mon = primary_monitor();
	auto *pfc = (PrivateFontCollection *)fonts_;
	std::wstring label = L"REC " + widen(format_duration((double)seconds));

	/* Opposite corner from the banner so they never overlap. */
	int x = position_ == "top-right" || position_ == "bottom-right" ? mon.left + (int)(16 * s)
									  : mon.right - w - (int)(16 * s);
	update_layered(rec_, x, mon.top + (int)(16 * s), w, h, 255, [&](Graphics &g) {
		GraphicsPath pill;
		rounded(pill, 0, 0, (REAL)w, (REAL)h, (REAL)h / 2);
		SolidBrush bg(Color(205, 0x0B, 0x0C, 0x0E));
		g.FillPath(&bg, &pill);
		SolidBrush dot(k_orange);
		g.FillEllipse(&dot, 11 * s, 10 * s, 8 * s, 8 * s);
		Font mono(mono_family_.c_str(), 12 * s, FontStyleBold, UnitPixel, pfc);
		SolidBrush text(k_text);
		g.DrawString(label.c_str(), -1, &mono, PointF(25 * s, 6 * s), &text);
	});
}

} // namespace hl

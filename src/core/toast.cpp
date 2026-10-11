#include "toast.h"
#include "gdiplus_include.h"

#include "../common/paths.h"
#include "../common/strings.h"

#include <functional>
#include <vector>

namespace hl {

using namespace Gdiplus;

static const wchar_t *k_class = L"HighlightOverlay";
static constexpr UINT_PTR k_hide_timer = 1;

static const Color k_text(255, 0xEC, 0xED, 0xEF);
static const Color k_muted(255, 0xA0, 0xA6, 0xB0);
static const Color k_orange(255, 0xFF, 0x5A, 0x1F);
static const Color k_ink(255, 0x0B, 0x0C, 0x0E);

static float ui_scale()
{
	return (float)GetDpiForSystem() / 96.0f;
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

static void update_layered(HWND hwnd, int x, int y, int w, int h, const std::function<void(Graphics &)> &draw)
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
	BLENDFUNCTION bf = {AC_SRC_OVER, 0, 255, AC_SRC_ALPHA};
	UpdateLayeredWindow(hwnd, screen, &dst, &size, mem, &src, 0, &bf, ULW_ALPHA);

	SelectObject(mem, old);
	DeleteObject(dib);
	DeleteDC(mem);
	ReleaseDC(nullptr, screen);
}

LRESULT CALLBACK Overlay::wndproc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
	if (msg == WM_TIMER && wp == k_hide_timer) {
		KillTimer(hwnd, k_hide_timer);
		ShowWindow(hwnd, SW_HIDE);
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
	toast_ = CreateWindowExW(ex, k_class, L"Highlight", WS_POPUP, 0, 0, 1, 1, nullptr, nullptr, inst, nullptr);
	rec_ = CreateWindowExW(ex, k_class, L"Highlight REC", WS_POPUP, 0, 0, 1, 1, nullptr, nullptr, inst, nullptr);
	load_fonts();
	return toast_ && rec_;
}

void Overlay::shutdown()
{
	if (toast_)
		DestroyWindow(toast_);
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

void Overlay::show_toast(const ToastContent &c, const std::string &position, int seconds)
{
	if (!toast_)
		return;
	content_ = c;
	position_ = position;
	paint_toast();
	SetWindowPos(toast_, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_SHOWWINDOW);
	SetTimer(toast_, k_hide_timer, (UINT)std::max(1, seconds) * 1000, nullptr);
}

static RECT primary_monitor()
{
	HMONITOR mon = MonitorFromPoint(POINT{0, 0}, MONITOR_DEFAULTTOPRIMARY);
	MONITORINFO mi = {sizeof(mi)};
	GetMonitorInfoW(mon, &mi);
	return mi.rcMonitor;
}

void Overlay::paint_toast()
{
	const float s = ui_scale();
	const int w = (int)(380 * s), h = (int)(92 * s);
	const int margin = (int)(24 * s);
	RECT mon = primary_monitor();

	int x = mon.right - w - margin, y = mon.top + margin;
	if (position_ == "top-left")
		x = mon.left + margin;
	else if (position_ == "bottom-right")
		y = mon.bottom - h - margin * 3;
	else if (position_ == "bottom-left") {
		x = mon.left + margin;
		y = mon.bottom - h - margin * 3;
	}

	auto *pfc = (PrivateFontCollection *)fonts_;
	const ToastContent c = content_;
	update_layered(toast_, x, y, w, h, [&](Graphics &g) {
		GraphicsPath bg;
		rounded(bg, 0.5f, 0.5f, (REAL)w - 1, (REAL)h - 1, 14 * s);
		SolidBrush bg_brush(Color(248, 0x11, 0x13, 0x17));
		g.FillPath(&bg_brush, &bg);
		Pen border(Color(255, 0x2A, 0x2E, 0x36), 1.0f * s);
		g.DrawPath(&border, &bg);

		/* icon */
		GraphicsPath box;
		rounded(box, 16 * s, 16 * s, 40 * s, 40 * s, 10 * s);
		const bool accent = c.kind != ToastKind::Error;
		SolidBrush box_brush(accent ? k_orange : Color(255, 0x3A, 0x3F, 0x48));
		g.FillPath(&box_brush, &box);
		Pen glyph(accent ? k_ink : k_text, 3.0f * s);
		glyph.SetStartCap(LineCapRound);
		glyph.SetEndCap(LineCapRound);
		glyph.SetLineJoin(LineJoinRound);
		SolidBrush ink(accent ? k_ink : k_text);
		if (c.kind == ToastKind::Saved) {
			PointF pts[] = {PointF(26 * s, 36.5f * s), PointF(32.5f * s, 43 * s), PointF(46 * s, 29 * s)};
			g.DrawLines(&glyph, pts, 3);
		} else if (c.kind == ToastKind::Bookmark) {
			PointF pts[] = {PointF(36 * s, 27 * s), PointF(45 * s, 36 * s), PointF(36 * s, 45 * s),
					PointF(27 * s, 36 * s)};
			g.FillPolygon(&ink, pts, 4);
		} else if (c.kind == ToastKind::Error) {
			g.DrawLine(&glyph, 36 * s, 26 * s, 36 * s, 38 * s);
			g.FillEllipse(&ink, 34.2f * s, 42 * s, 3.6f * s, 3.6f * s);
		} else {
			g.FillRectangle(&ink, 25 * s, 26 * s, 4.5f * s, 20 * s);
			g.FillRectangle(&ink, 42.5f * s, 26 * s, 4.5f * s, 20 * s);
			PointF tri[] = {PointF(31.5f * s, 30 * s), PointF(31.5f * s, 42 * s), PointF(40.5f * s, 36 * s)};
			g.FillPolygon(&ink, tri, 3);
		}

		Font title(title_family_.c_str(), 15 * s, FontStyleBold, UnitPixel, pfc);
		Font mono(mono_family_.c_str(), 12.5f * s, FontStyleRegular, UnitPixel, pfc);
		Font mono_small(mono_family_.c_str(), 11 * s, FontStyleRegular, UnitPixel, pfc);
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
			rounded(chip, right - kw, 14 * s, kw, kh, 5 * s);
			SolidBrush chip_bg(Color(255, 0x1D, 0x20, 0x26));
			Pen chip_border(Color(255, 0x33, 0x38, 0x42), 1.0f * s);
			g.FillPath(&chip_bg, &chip);
			g.DrawPath(&chip_border, &chip);
			g.DrawString(key.c_str(), -1, &mono_small, PointF(right - kw + 5 * s, 16.5f * s), &muted);
			right -= kw + 8 * s;
		}

		const REAL tx = 70 * s;
		std::wstring t = widen(c.title), l1 = widen(c.line1), l2 = widen(c.line2);
		g.DrawString(t.c_str(), -1, &title, RectF(tx, 13 * s, right - tx, 22 * s), &one_line, &text);
		g.DrawString(l1.c_str(), -1, &mono, RectF(tx, 38 * s, (REAL)w - tx - 16 * s, 20 * s), &one_line, &text);
		StringFormat path_fmt;
		path_fmt.SetFormatFlags(StringFormatFlagsNoWrap);
		path_fmt.SetTrimming(StringTrimmingEllipsisPath);
		g.DrawString(l2.c_str(), -1, &mono_small, RectF(tx, 60 * s, (REAL)w - tx - 16 * s, 18 * s), &path_fmt,
			     &muted);
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

	update_layered(rec_, mon.left + (int)(16 * s), mon.top + (int)(16 * s), w, h, [&](Graphics &g) {
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

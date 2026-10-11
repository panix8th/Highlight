/* highlight-ui.exe - the window. Started on demand by the core (or the
 * launcher) and fully exits when closed, so it costs no memory in the background. */

#include "app_ui.h"
#include "theme.h"

#include "../common/log.h"

#include <imgui.h>
#include <imgui_impl_dx11.h>
#include <imgui_impl_win32.h>

#include <windows.h>
#include <windowsx.h>
#include <objbase.h>
#include <d3d11.h>
#include <dwmapi.h>

#include <algorithm>

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND, UINT, WPARAM, LPARAM);

namespace {

ID3D11Device *g_device = nullptr;
ID3D11DeviceContext *g_ctx = nullptr;
IDXGISwapChain *g_swap = nullptr;
ID3D11RenderTargetView *g_rtv = nullptr;
UINT g_resize_w = 0, g_resize_h = 0;
ui::AppUI *g_app = nullptr;
bool g_rebuild_fonts = false;
float g_pending_scale = 1.0f;

void create_rtv()
{
	ID3D11Texture2D *back = nullptr;
	g_swap->GetBuffer(0, IID_PPV_ARGS(&back));
	if (back) {
		g_device->CreateRenderTargetView(back, nullptr, &g_rtv);
		back->Release();
	}
}

void release_rtv()
{
	if (g_rtv) {
		g_rtv->Release();
		g_rtv = nullptr;
	}
}

bool create_device(HWND hwnd)
{
	DXGI_SWAP_CHAIN_DESC sd = {};
	sd.BufferCount = 2;
	sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
	sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
	sd.OutputWindow = hwnd;
	sd.SampleDesc.Count = 1;
	sd.Windowed = TRUE;
	sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
	const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_0};
	D3D_FEATURE_LEVEL got;
	HRESULT hr = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, levels, 2,
						   D3D11_SDK_VERSION, &sd, &g_swap, &g_device, &got, &g_ctx);
	if (hr == DXGI_ERROR_UNSUPPORTED)
		hr = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, levels, 2,
						   D3D11_SDK_VERSION, &sd, &g_swap, &g_device, &got, &g_ctx);
	if (FAILED(hr))
		return false;
	create_rtv();
	return true;
}

void destroy_device()
{
	release_rtv();
	if (g_swap)
		g_swap->Release();
	if (g_ctx)
		g_ctx->Release();
	if (g_device)
		g_device->Release();
	g_swap = nullptr;
	g_ctx = nullptr;
	g_device = nullptr;
}

LRESULT CALLBACK wndproc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
	switch (msg) {
	case WM_NCCALCSIZE:
		if (wp) {
			/* Remove the standard frame but keep resizing, snapping and the shadow. */
			if (IsZoomed(hwnd)) {
				auto *p = (NCCALCSIZE_PARAMS *)lp;
				UINT dpi = GetDpiForWindow(hwnd);
				int fx = GetSystemMetricsForDpi(SM_CXFRAME, dpi) + GetSystemMetricsForDpi(SM_CXPADDEDBORDER, dpi);
				int fy = GetSystemMetricsForDpi(SM_CYFRAME, dpi) + GetSystemMetricsForDpi(SM_CXPADDEDBORDER, dpi);
				p->rgrc[0].left += fx;
				p->rgrc[0].right -= fx;
				p->rgrc[0].top += fy;
				p->rgrc[0].bottom -= fy;
			}
			return 0;
		}
		break;
	case WM_NCHITTEST: {
		POINT pt = {GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
		RECT wr;
		GetWindowRect(hwnd, &wr);
		const int border = (int)(8 * ui::dpi_scale);
		if (!IsZoomed(hwnd)) {
			bool l = pt.x < wr.left + border, r = pt.x >= wr.right - border;
			bool t = pt.y < wr.top + border, b = pt.y >= wr.bottom - border;
			if (t && l)
				return HTTOPLEFT;
			if (t && r)
				return HTTOPRIGHT;
			if (b && l)
				return HTBOTTOMLEFT;
			if (b && r)
				return HTBOTTOMRIGHT;
			if (l)
				return HTLEFT;
			if (r)
				return HTRIGHT;
			if (t)
				return HTTOP;
			if (b)
				return HTBOTTOM;
		}
		POINT client = pt;
		ScreenToClient(hwnd, &client);
		if (g_app && g_app->hit_caption(client))
			return HTCAPTION;
		return HTCLIENT;
	}
	case WM_GETMINMAXINFO: {
		auto *mm = (MINMAXINFO *)lp;
		mm->ptMinTrackSize.x = (LONG)(960 * ui::dpi_scale);
		mm->ptMinTrackSize.y = (LONG)(640 * ui::dpi_scale);
		return 0;
	}
	case WM_SIZE:
		if (wp != SIZE_MINIMIZED) {
			g_resize_w = LOWORD(lp);
			g_resize_h = HIWORD(lp);
		}
		return 0;
	case WM_DPICHANGED: {
		g_pending_scale = HIWORD(wp) / 96.0f;
		g_rebuild_fonts = true;
		auto *r = (RECT *)lp;
		SetWindowPos(hwnd, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top,
			     SWP_NOZORDER | SWP_NOACTIVATE);
		return 0;
	}
	case WM_SYSCOMMAND:
		if ((wp & 0xfff0) == SC_KEYMENU)
			return 0;
		break;
	case WM_CLOSE:
		PostQuitMessage(0);
		return 0;
	case WM_DESTROY:
		PostQuitMessage(0);
		return 0;
	}
	if (ImGui_ImplWin32_WndProcHandler(hwnd, msg, wp, lp))
		return true;
	return DefWindowProcW(hwnd, msg, wp, lp);
}

} // namespace

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, PWSTR, int show)
{
	HANDLE mutex = CreateMutexW(nullptr, TRUE, L"Local\\HighlightUI");
	if (GetLastError() == ERROR_ALREADY_EXISTS) {
		/* Ask the running window to come forward through the core. */
		hl::ipc::Client c;
		if (c.connect(1500))
			c.send({{"cmd", "open_ui"}});
		return 0;
	}

	SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
	CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
	hl::log::init("ui");

	WNDCLASSEXW wc = {sizeof(wc)};
	wc.style = CS_CLASSDC;
	wc.lpfnWndProc = wndproc;
	wc.hInstance = inst;
	wc.hIcon = LoadIconW(inst, MAKEINTRESOURCEW(1));
	wc.hIconSm = wc.hIcon;
	wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
	wc.lpszClassName = L"HighlightWindow";
	RegisterClassExW(&wc);

	POINT origin = {0, 0};
	HMONITOR mon = MonitorFromPoint(origin, MONITOR_DEFAULTTOPRIMARY);
	MONITORINFO mi = {sizeof(mi)};
	GetMonitorInfoW(mon, &mi);
	UINT dpi_x = 96, dpi_y = 96;
	{
		HDC dc = GetDC(nullptr);
		dpi_x = (UINT)GetDeviceCaps(dc, LOGPIXELSX);
		ReleaseDC(nullptr, dc);
		(void)dpi_y;
	}
	float scale = dpi_x / 96.0f;
	int ww = std::min((int)(1320 * scale), (int)(mi.rcWork.right - mi.rcWork.left) - 40);
	int wh = std::min((int)(860 * scale), (int)(mi.rcWork.bottom - mi.rcWork.top) - 40);
	int wx = mi.rcWork.left + ((mi.rcWork.right - mi.rcWork.left) - ww) / 2;
	int wy = mi.rcWork.top + ((mi.rcWork.bottom - mi.rcWork.top) - wh) / 2;

	HWND hwnd = CreateWindowExW(WS_EX_APPWINDOW, wc.lpszClassName, L"Highlight", WS_OVERLAPPEDWINDOW, wx, wy, ww, wh,
				    nullptr, nullptr, inst, nullptr);
	if (!hwnd)
		return 1;
	scale = GetDpiForWindow(hwnd) / 96.0f;

	MARGINS margins = {0, 0, 1, 0};
	DwmExtendFrameIntoClientArea(hwnd, &margins);
	BOOL dark = TRUE;
	DwmSetWindowAttribute(hwnd, 20 /* DWMWA_USE_IMMERSIVE_DARK_MODE */, &dark, sizeof(dark));
	SetWindowPos(hwnd, nullptr, 0, 0, 0, 0, SWP_FRAMECHANGED | SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER);

	if (!create_device(hwnd)) {
		MessageBoxW(hwnd, L"Highlight needs DirectX 11.", L"Highlight", MB_ICONERROR);
		return 1;
	}

	IMGUI_CHECKVERSION();
	ImGui::CreateContext();
	ImGuiIO &io = ImGui::GetIO();
	io.IniFilename = nullptr;
	io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
	ImGui_ImplWin32_Init(hwnd);
	ImGui_ImplDX11_Init(g_device, g_ctx);
	ui::textures_init(g_device, g_ctx);
	ui::setup_theme(scale);

	ui::AppUI app;
	g_app = &app;
	app.init(hwnd);

	ShowWindow(hwnd, show == SW_HIDE ? SW_SHOWNORMAL : show);
	UpdateWindow(hwnd);

	bool running = true;
	ULONGLONG last_input = GetTickCount64();
	while (running) {
		/* Sleep until input arrives, unless something is animating. */
		bool fast = app.wants_fast_refresh() || GetTickCount64() - last_input < 600;
		if (!fast)
			MsgWaitForMultipleObjects(0, nullptr, FALSE, 250, QS_ALLINPUT);

		MSG msg;
		while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
			if (msg.message == WM_QUIT)
				running = false;
			if ((msg.message >= WM_MOUSEFIRST && msg.message <= WM_MOUSELAST) ||
			    (msg.message >= WM_KEYFIRST && msg.message <= WM_KEYLAST) || msg.message == WM_SIZE)
				last_input = GetTickCount64();
			TranslateMessage(&msg);
			DispatchMessageW(&msg);
		}
		if (!running || app.should_close())
			break;
		if (IsIconic(hwnd)) {
			Sleep(50);
			ImGui_ImplDX11_NewFrame();
			ImGui_ImplWin32_NewFrame();
			ImGui::NewFrame();
			app.frame(); /* keep IPC and jobs moving */
			ImGui::EndFrame();
			continue;
		}

		if (g_resize_w && g_resize_h) {
			release_rtv();
			g_swap->ResizeBuffers(0, g_resize_w, g_resize_h, DXGI_FORMAT_UNKNOWN, 0);
			g_resize_w = g_resize_h = 0;
			create_rtv();
		}
		if (g_rebuild_fonts) {
			g_rebuild_fonts = false;
			ImGui_ImplDX11_InvalidateDeviceObjects();
			ui::setup_theme(g_pending_scale);
			ImGui_ImplDX11_CreateDeviceObjects();
		}

		ImGui_ImplDX11_NewFrame();
		ImGui_ImplWin32_NewFrame();
		ImGui::NewFrame();
		app.frame();
		ImGui::Render();

		const float clear[4] = {0.043f, 0.047f, 0.055f, 1.0f};
		g_ctx->OMSetRenderTargets(1, &g_rtv, nullptr);
		g_ctx->ClearRenderTargetView(g_rtv, clear);
		ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
		g_swap->Present(1, 0);
	}

	app.shutdown();
	g_app = nullptr;
	ImGui_ImplDX11_Shutdown();
	ImGui_ImplWin32_Shutdown();
	ImGui::DestroyContext();
	destroy_device();
	DestroyWindow(hwnd);
	hl::log::shutdown();
	CoUninitialize();
	if (mutex) {
		ReleaseMutex(mutex);
		CloseHandle(mutex);
	}
	return 0;
}

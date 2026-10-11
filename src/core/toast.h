#pragma once

/* The in-game pop-up ("Clip saved") and the small REC indicator. Both are
 * click-through, never take focus, and stay on top of borderless games. */

#include <windows.h>

#include <cstdint>
#include <string>

namespace hl {

enum class ToastKind { Saved, Bookmark, Error, Info };

struct ToastContent {
	ToastKind kind = ToastKind::Saved;
	std::string title;
	std::string line1;
	std::string line2;
	std::string key; /* hotkey chip, e.g. "F8" */
};

class Overlay {
public:
	bool init(HINSTANCE inst);
	void shutdown();

	void show_toast(const ToastContent &c, const std::string &position, int seconds);
	void set_recording(bool on, int64_t seconds);

private:
	static LRESULT CALLBACK wndproc(HWND, UINT, WPARAM, LPARAM);
	void paint_toast();
	void paint_rec(int64_t seconds);
	void load_fonts();

	HWND toast_ = nullptr;
	HWND rec_ = nullptr;
	ToastContent content_;
	std::string position_ = "top-right";
	void *fonts_ = nullptr; /* Gdiplus::PrivateFontCollection */
	std::wstring title_family_;
	std::wstring body_family_;
	std::wstring mono_family_;
};

} // namespace hl

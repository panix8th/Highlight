#pragma once

/* The in-game banner ("Saving clip…" -> "Clip saved") and the optional REC
 * indicator. Both are click-through, never take focus, and stay on top of
 * borderless and windowed games. The banner slides in, shows a progress bar
 * while a clip is still being written, then slides out. */

#include <windows.h>

#include <cstdint>
#include <string>

namespace hl {

enum class ToastKind { Saved, Bookmark, Error, Info, Working };

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

	/* A finished card (saved, bookmark, error, info). Replaces whatever is showing. */
	void show_toast(const ToastContent &c, const std::string &position, int seconds);
	/* An animated "working" card: the bar fills over expected_seconds, then keeps
	 * moving until show_toast() replaces it with the result. */
	void show_progress(const ToastContent &c, const std::string &position, double expected_seconds);
	void set_recording(bool on, int64_t seconds);

private:
	static LRESULT CALLBACK wndproc(HWND, UINT, WPARAM, LPARAM);
	void begin(const ToastContent &c, const std::string &position);
	void tick();
	void paint(float visibility);
	void paint_rec(int64_t seconds);
	void load_fonts();

	HWND toast_ = nullptr;
	HWND rec_ = nullptr;
	ToastContent content_;
	std::string position_ = "top-left";
	void *fonts_ = nullptr; /* Gdiplus::PrivateFontCollection */
	std::wstring title_family_;
	std::wstring body_family_;
	std::wstring mono_family_;

	/* animation state (seconds, steady clock) */
	bool visible_ = false;
	double shown_at_ = 0;
	double hide_at_ = 0;
	double hiding_since_ = 0;
	double progress_start_ = 0;
	double progress_secs_ = 0;
	double display_secs_ = 3;
};

} // namespace hl

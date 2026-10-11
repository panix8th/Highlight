#include "hotkeys.h"

#include <windows.h>

namespace hl {

static bool key_down(int vk)
{
	return (GetAsyncKeyState(vk) & 0x8000) != 0;
}

Hotkeys::~Hotkeys()
{
	stop();
}

void Hotkeys::start(Fn fn)
{
	fn_ = std::move(fn);
	running_ = true;
	thread_ = std::thread(&Hotkeys::loop, this);
}

void Hotkeys::stop()
{
	running_ = false;
	if (thread_.joinable())
		thread_.join();
}

void Hotkeys::set_bindings(std::vector<Binding> bindings)
{
	std::lock_guard lock(mutex_);
	bindings_ = std::move(bindings);
	down_.assign(bindings_.size(), true); /* ignore keys already held when bindings change */
}

void Hotkeys::loop()
{
	SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_ABOVE_NORMAL);
	while (running_) {
		Sleep(10);

		const bool ctrl = key_down(VK_CONTROL);
		const bool shift = key_down(VK_SHIFT);
		const bool alt = key_down(VK_MENU);
		const bool win = key_down(VK_LWIN) || key_down(VK_RWIN);

		std::vector<std::pair<int, bool>> fired;
		{
			std::lock_guard lock(mutex_);
			for (size_t i = 0; i < bindings_.size(); i++) {
				const Binding &b = bindings_[i];
				if (b.key.empty())
					continue;
				bool pressed = key_down((int)b.key.vk);
				if (pressed && !b.hold)
					pressed = ctrl == b.key.ctrl && shift == b.key.shift && alt == b.key.alt &&
						  win == b.key.win;
				else if (pressed && b.hold)
					pressed = (!b.key.ctrl || ctrl) && (!b.key.shift || shift) && (!b.key.alt || alt) &&
						  (!b.key.win || win);

				if (pressed != down_[i]) {
					down_[i] = pressed;
					if (pressed || b.hold)
						fired.emplace_back(b.id, pressed);
				}
			}
		}

		if (!enabled_)
			continue;
		for (auto &[id, pressed] : fired)
			if (fn_)
				fn_(id, pressed);
	}
}

} // namespace hl

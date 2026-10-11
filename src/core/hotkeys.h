#pragma once

/* Global hotkeys that keep working inside fullscreen games. Like OBS, this polls
 * the async key state (~100 Hz) instead of RegisterHotKey, which also gives us
 * key-up events (needed for push-to-talk) and mouse side buttons. */

#include "../common/settings.h"

#include <atomic>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>

namespace hl {

class Hotkeys {
public:
	struct Binding {
		int id = 0;
		Hotkey key;
		bool hold = false; /* report release too, ignore extra modifiers */
	};
	using Fn = std::function<void(int id, bool pressed)>;

	~Hotkeys();
	void start(Fn fn);
	void stop();
	void set_bindings(std::vector<Binding> bindings);
	void set_enabled(bool enabled) { enabled_ = enabled; }

private:
	void loop();

	Fn fn_;
	std::thread thread_;
	std::atomic<bool> running_{false};
	std::atomic<bool> enabled_{true};
	std::mutex mutex_;
	std::vector<Binding> bindings_;
	std::vector<bool> down_;
};

} // namespace hl

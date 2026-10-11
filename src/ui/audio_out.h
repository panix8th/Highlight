#pragma once

/* Minimal audio output for the editor preview (waveOut, 48 kHz stereo float). */

#include <atomic>
#include <functional>
#include <thread>

namespace ui {

class AudioOut {
public:
	using Pull = std::function<size_t(float *dst, size_t frames)>;

	~AudioOut();
	bool start(Pull pull);
	void stop();

private:
	void loop();

	Pull pull_;
	std::thread thread_;
	std::atomic<bool> running_{false};
};

} // namespace ui

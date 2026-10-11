#pragma once

/* Preview player for the clip editor. Decodes on a worker thread, hands BGRA
 * frames to the UI at the right time and mixes the selected audio tracks into
 * 48 kHz stereo float for the audio device. */

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace hl::media {

class Player {
public:
	Player();
	~Player();
	Player(const Player &) = delete;
	Player &operator=(const Player &) = delete;

	bool open(const std::string &path, int max_w, int max_h, std::string *err);
	void close();
	bool is_open() const { return open_; }

	double duration() const { return duration_; }
	double position() const;
	bool playing() const { return playing_; }

	void play();
	void pause();
	void seek(double t);
	void set_audio_streams(const std::vector<int> &streams);
	void set_volume(float v) { volume_ = v; }

	/* UI thread: the newest frame that is due. Returns false if nothing changed. */
	bool take_frame(std::vector<uint8_t> &bgra, int &w, int &h);

	/* Audio device thread: fills interleaved stereo float frames, returns how many were written. */
	size_t read_audio(float *dst, size_t frames);

private:
	struct Impl;
	void worker();

	std::unique_ptr<Impl> impl_;
	std::thread thread_;
	mutable std::mutex mutex_;
	std::condition_variable cv_;
	std::atomic<bool> open_{false};
	std::atomic<bool> running_{false};
	std::atomic<bool> playing_{false};
	std::atomic<float> volume_{1.0f};
	double duration_ = 0;

	/* clock */
	double clock_pos_ = 0;
	int64_t clock_start_us_ = 0;

	/* commands */
	bool seek_pending_ = false;
	double seek_target_ = 0;
	bool streams_changed_ = false;
	std::vector<int> audio_streams_;

	/* output */
	struct Frame {
		double pts = 0;
		int w = 0, h = 0;
		std::vector<uint8_t> bgra;
	};
	std::deque<Frame> frames_;
	Frame still_;
	bool still_ready_ = false;
	std::deque<float> audio_;
	bool eof_ = false;
};

} // namespace hl::media

#include "sound.h"

#include <windows.h>
#include <mmsystem.h>

#include <cmath>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <vector>

namespace hl {

namespace {

struct Tone {
	double freq;
	double seconds;
};

std::vector<uint8_t> make_wav(const std::vector<Tone> &tones, double volume)
{
	const int rate = 44100;
	std::vector<int16_t> samples;
	for (const auto &t : tones) {
		int n = (int)(t.seconds * rate);
		for (int i = 0; i < n; i++) {
			double env = std::min(1.0, i / (rate * 0.004)) * std::exp(-3.5 * i / (double)n);
			double s = std::sin(2.0 * 3.14159265358979 * t.freq * i / rate) * env * volume;
			samples.push_back((int16_t)(s * 32767 * 0.6));
		}
	}

	const uint32_t data_bytes = (uint32_t)(samples.size() * 2);
	std::vector<uint8_t> wav(44 + data_bytes);
	auto put32 = [&](size_t off, uint32_t v) { memcpy(&wav[off], &v, 4); };
	auto put16 = [&](size_t off, uint16_t v) { memcpy(&wav[off], &v, 2); };
	memcpy(&wav[0], "RIFF", 4);
	put32(4, 36 + data_bytes);
	memcpy(&wav[8], "WAVEfmt ", 8);
	put32(16, 16);
	put16(20, 1);
	put16(22, 1);
	put32(24, rate);
	put32(28, rate * 2);
	put16(32, 2);
	put16(34, 16);
	memcpy(&wav[36], "data", 4);
	put32(40, data_bytes);
	memcpy(&wav[44], samples.data(), data_bytes);
	return wav;
}

} // namespace

void play_cue(Cue cue, int volume)
{
	if (volume <= 0)
		return;
	static std::mutex mutex;
	static std::vector<uint8_t> current; /* must outlive async playback */

	std::vector<Tone> tones;
	switch (cue) {
	case Cue::Saved:
		tones = {{880, 0.07}, {1318.5, 0.16}};
		break;
	case Cue::Bookmark:
		tones = {{1046.5, 0.09}};
		break;
	case Cue::Error:
		tones = {{329.6, 0.12}, {246.9, 0.2}};
		break;
	case Cue::RecordStart:
		tones = {{659.3, 0.08}, {987.8, 0.12}};
		break;
	case Cue::RecordStop:
		tones = {{987.8, 0.08}, {659.3, 0.12}};
		break;
	}

	std::lock_guard lock(mutex);
	PlaySoundW(nullptr, nullptr, 0);
	current = make_wav(tones, volume / 100.0);
	PlaySoundW((LPCWSTR)current.data(), nullptr, SND_MEMORY | SND_ASYNC | SND_NODEFAULT);
}

} // namespace hl

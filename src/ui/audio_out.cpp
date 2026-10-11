#include "audio_out.h"

#include <windows.h>
#include <mmreg.h>
#include <mmsystem.h>

#include <cstring>
#include <vector>

namespace ui {

static constexpr int kRate = 48000;
static constexpr int kFrames = 1024; /* per buffer, ~21 ms */
static constexpr int kBuffers = 4;

AudioOut::~AudioOut()
{
	stop();
}

bool AudioOut::start(Pull pull)
{
	stop();
	pull_ = std::move(pull);
	running_ = true;
	thread_ = std::thread(&AudioOut::loop, this);
	return true;
}

void AudioOut::stop()
{
	running_ = false;
	if (thread_.joinable())
		thread_.join();
}

void AudioOut::loop()
{
	WAVEFORMATEX fmt = {};
	fmt.wFormatTag = WAVE_FORMAT_IEEE_FLOAT;
	fmt.nChannels = 2;
	fmt.nSamplesPerSec = kRate;
	fmt.wBitsPerSample = 32;
	fmt.nBlockAlign = fmt.nChannels * fmt.wBitsPerSample / 8;
	fmt.nAvgBytesPerSec = fmt.nSamplesPerSec * fmt.nBlockAlign;

	HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
	HWAVEOUT out = nullptr;
	if (waveOutOpen(&out, WAVE_MAPPER, &fmt, (DWORD_PTR)event, 0, CALLBACK_EVENT) != MMSYSERR_NOERROR) {
		CloseHandle(event);
		return;
	}

	std::vector<std::vector<float>> data(kBuffers, std::vector<float>(kFrames * 2));
	std::vector<WAVEHDR> hdr(kBuffers);
	for (int i = 0; i < kBuffers; i++) {
		hdr[i] = {};
		hdr[i].lpData = (LPSTR)data[i].data();
		hdr[i].dwBufferLength = (DWORD)(data[i].size() * sizeof(float));
		waveOutPrepareHeader(out, &hdr[i], sizeof(WAVEHDR));
		hdr[i].dwFlags |= WHDR_DONE;
	}

	while (running_) {
		for (int i = 0; i < kBuffers; i++) {
			if (!(hdr[i].dwFlags & WHDR_DONE))
				continue;
			size_t got = pull_ ? pull_(data[i].data(), kFrames) : 0;
			if (got < (size_t)kFrames)
				memset(data[i].data() + got * 2, 0, (kFrames - got) * 2 * sizeof(float));
			hdr[i].dwFlags &= ~WHDR_DONE;
			waveOutWrite(out, &hdr[i], sizeof(WAVEHDR));
		}
		WaitForSingleObject(event, 20);
	}

	waveOutReset(out);
	for (int i = 0; i < kBuffers; i++)
		waveOutUnprepareHeader(out, &hdr[i], sizeof(WAVEHDR));
	waveOutClose(out);
	CloseHandle(event);
}

} // namespace ui

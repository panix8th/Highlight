#pragma once

/* Writes already-encoded packets (straight from the OBS encoders, via the
 * disk ring) into an MP4/MKV/MOV file without re-encoding. */

#include <cstdint>
#include <string>
#include <vector>

struct AVFormatContext;

namespace hl::media {

struct StreamDesc {
	bool video = false;
	std::string codec; /* "h264" | "hevc" | "av1" | "aac" | "opus" */
	std::string title;
	std::vector<uint8_t> extradata;
	int64_t bitrate = 0;

	/* video */
	int width = 0;
	int height = 0;
	int fps_num = 60;
	int fps_den = 1;
	bool hdr = false;

	/* audio */
	int sample_rate = 48000;
	int channels = 2;
	int frame_size = 1024;
};

struct Chapter {
	double start = 0; /* seconds */
	double end = 0;
	std::string title;
};

class Mp4Writer {
public:
	~Mp4Writer();

	/* format: "mp4" | "mov" | "matroska". The file is written to path + ".partial"
	 * and renamed on finish(), so a crash never leaves a broken clip behind. */
	bool open(const std::string &path, const std::string &format, const std::vector<StreamDesc> &streams,
		  const std::vector<Chapter> &chapters, std::string *err);

	/* Timestamps are in units of 1/timebase_den seconds (as OBS packets are). */
	bool write(int stream, const uint8_t *data, size_t size, int64_t pts, int64_t dts, int32_t timebase_den,
		   bool keyframe);

	bool finish(std::string *err);
	void abort();

	uint64_t bytes_written() const { return bytes_; }

private:
	AVFormatContext *ctx_ = nullptr;
	std::string path_;
	std::string temp_path_;
	std::vector<int64_t> last_dts_;
	uint64_t bytes_ = 0;
	bool header_written_ = false;
};

} // namespace hl::media

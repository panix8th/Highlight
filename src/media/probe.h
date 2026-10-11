#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace hl::media {

struct MediaInfo {
	bool ok = false;
	std::string error;

	double duration = 0;
	uint64_t file_size = 0;

	int video_index = -1;
	std::string video_codec;
	int width = 0;
	int height = 0;
	double fps = 0;
	int64_t video_bitrate = 0;

	struct Audio {
		int index = -1;
		std::string title;
		std::string codec;
		int sample_rate = 0;
		int channels = 0;
	};
	std::vector<Audio> audio;

	struct Mark {
		double time = 0;
		std::string title;
	};
	std::vector<Mark> chapters;
};

MediaInfo probe(const std::string &path);

} // namespace hl::media

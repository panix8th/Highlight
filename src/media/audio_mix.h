#pragma once

/* Re-mixes the audio of a clip without touching its video: video packets are
 * copied as they are, every audio track is decoded, set to its own volume and
 * encoded again, and a new "Mix" track (the sum of all of them) goes first so
 * players that only play track 1 hear the new balance. */

#include <string>
#include <vector>

namespace hl::media {

struct AudioMixOptions {
	/* One entry per audio stream of the input, in stream order. 1.0 = unchanged. */
	std::vector<float> gains;
	bool add_mix = true; /* put a mixed track first (only when there are 2+ tracks) */
	int bitrate = 192000;
	std::string format = "mp4"; /* mp4 | mov | matroska */
};

bool remix_audio(const std::string &in, const std::string &out, const AudioMixOptions &opt, std::string *err);

} // namespace hl::media

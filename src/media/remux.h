#pragma once

#include <string>
#include <vector>

namespace hl::media {

/* Copies all streams into a new container without re-encoding.
 * format: "mp4" | "mov" | "matroska". */
bool remux(const std::string &in, const std::string &out, const std::string &format, std::string *err);

struct TrimOptions {
	double start = 0; /* seconds; the cut snaps back to the keyframe at or before this */
	double end = -1;  /* seconds; -1 = to the end */
	std::vector<int> audio_streams; /* input stream indices to keep; empty = all */
	std::string format = "mp4";
};

/* Lossless trim. Returns the actual start time used (keyframe aligned). */
bool trim_lossless(const std::string &in, const std::string &out, const TrimOptions &opt, std::string *err,
		   double *actual_start = nullptr);

} // namespace hl::media

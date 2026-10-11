#pragma once

/* "Share copy" export: re-encodes a clip into a separate, smaller file for
 * Discord and friends. Two modes:
 *   - target size: picks the best resolution/frame rate that fits the limit
 *     and encodes with two-pass-quality settings on the GPU (NVENC);
 *   - best quality: constant-quality encode that looks the same as the
 *     original but is usually several times smaller.
 * The original clip is never modified. */

#include "probe.h"

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace hl::media {

struct ShareRequest {
	std::string input;
	std::string output;
	double start = 0; /* trim, seconds */
	double end = -1;
	std::vector<int> audio_streams; /* input stream indices to mix; empty = first audio stream */
	std::string codec = "h264";     /* h264 | hevc | av1 */
	int64_t target_bytes = 0;       /* 0 = best quality, no limit */
};

struct SharePlan {
	int width = 0;
	int height = 0;
	int fps = 60;
	int64_t video_bitrate = 0; /* bits/s; 0 = constant quality */
	int cq = 0;
	int audio_bitrate = 128000;
	double duration = 0;
	int64_t target_bytes = 0;
	int64_t estimated_bytes = 0;
	std::string codec;
	std::string quality; /* Identical | Excellent | Great | Soft */
	int quality_level = 0; /* 1..4 */
};

SharePlan plan_share(const MediaInfo &info, const ShareRequest &req);

/* progress: 0..1; return false to cancel. Retries once with a lower bitrate if
 * the result overshoots the size limit. */
bool run_share(const ShareRequest &req, SharePlan plan, const std::function<bool(float)> &progress, std::string *err,
	       int64_t *out_bytes);

/* Puts the file on the clipboard as a file drop, so Ctrl+V pastes it into Discord. */
bool copy_file_to_clipboard(const std::string &path);

} // namespace hl::media

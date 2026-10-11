#pragma once

/* Turns a span of the rolling buffer into a clip file. Kept free of libobs so the
 * whole buffer -> MP4 path can be tested on its own (tests/clip_pipeline_test.cpp). */

#include "disk_ring.h"

#include "../media/mp4_writer.h"

#include <cstdint>
#include <string>
#include <vector>

namespace hl {

constexpr size_t kMaxClipTracks = 6; /* MAX_AUDIO_MIXES in libobs */

struct ClipRequest {
	uint64_t id = 0;
	int64_t press_sys_usec = 0;
	int pre_seconds = 60;
	int post_seconds = 0;
	std::string path;   /* final file path, UTF-8 */
	std::string format; /* "mp4" | "mov" | "matroska" */
	std::string game;
};

struct ClipResult {
	uint64_t id = 0;
	bool ok = false;
	std::string path;
	std::string game;
	std::string error;
	double duration = 0;
	uint64_t bytes = 0;
	int bookmarks = 0;
};

struct ClipJob {
	ClipRequest req;
	std::vector<media::StreamDesc> streams;
	std::vector<int> track_to_stream; /* audio track index -> output stream, -1 = not saved */
	std::vector<int64_t> bookmarks;   /* system time, microseconds */
};

/* Reads [press - pre, press + post] from the ring and writes job.req.path. */
ClipResult write_clip(DiskRing &ring, const ClipJob &job);

/* Pulls the decoder setup (H.264 SPS/PPS, HEVC VPS/SPS/PPS, AV1 sequence header)
 * out of an encoded packet. Encoders often put it only in the very first packet,
 * so the buffer keeps a copy: without it the muxer has nothing to describe the
 * stream with once that packet has left the buffer. Returns false if the packet
 * carries none. */
bool extract_codec_headers(const std::string &codec, const uint8_t *data, size_t size, std::vector<uint8_t> *out);

} // namespace hl

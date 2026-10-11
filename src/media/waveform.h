#pragma once

#include <string>
#include <vector>

namespace hl::media {

/* Peak envelope of one audio stream over the whole file: `buckets` values in 0..1. */
bool audio_peaks(const std::string &path, int stream_index, int buckets, std::vector<float> &out);

} // namespace hl::media

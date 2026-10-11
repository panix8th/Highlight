#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace hl::media {

/* Decodes the frame at `at` seconds, scaled to fit max_w x max_h, as BGRA. */
bool decode_thumbnail(const std::string &path, double at, int max_w, int max_h, std::vector<uint8_t> &bgra, int &w,
		      int &h, std::string *err = nullptr);

} // namespace hl::media

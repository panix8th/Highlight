#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace hl {

/* Writes 32-bit BGRA pixels with the Windows Imaging Component.
 * format: "png" or "jpg". Call from a thread with COM initialized (or not -
 * the function initializes COM for itself if needed). */
bool write_image(const std::wstring &path, const uint8_t *bgra, int width, int height, int stride,
		 const std::string &format, float jpeg_quality = 0.85f);

/* Decodes any WIC-supported image into tightly packed BGRA. */
bool read_image(const std::wstring &path, std::vector<uint8_t> &bgra, int &width, int &height);

} // namespace hl

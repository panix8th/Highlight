#pragma once

#include <cstdint>
#include <string>

namespace hl {

std::wstring widen(const std::string &utf8);
std::string narrow(const std::wstring &wide);

/* 65.0 -> "1:05", 6130 -> "1:42:10" */
std::string format_duration(double seconds);

/* 341000000 -> "325 MB", 1.2e9 -> "1.1 GB" */
std::string format_bytes(uint64_t bytes);

/* Removes characters Windows does not allow in file names. */
std::string sanitize_filename(const std::string &name);

std::string to_lower(std::string s);

} // namespace hl

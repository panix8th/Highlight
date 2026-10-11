#pragma once

#include <string>

namespace hl {

/* Turns a game's executable (and window title as a fallback) into a readable name:
 * "VALORANT-Win64-Shipping.exe" -> "VALORANT", "cs2.exe" -> "Counter-Strike 2". */
std::string game_display_name(const std::string &executable, const std::string &window_title);

} // namespace hl

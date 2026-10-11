#pragma once

#include <string>

namespace hl {

/* Turns a game's executable (and window title as a fallback) into a readable name:
 * "VALORANT-Win64-Shipping.exe" -> "VALORANT", "cs2.exe" -> "Counter-Strike 2". */
std::string game_display_name(const std::string &executable, const std::string &window_title);

/* True for games Highlight knows by name. Used to capture them even when they run
 * in a window, which automatic (fullscreen) detection skips. Java only counts when
 * the window is Minecraft (or a known Minecraft client). */
bool is_known_game(const std::string &executable, const std::string &window_title);

} // namespace hl

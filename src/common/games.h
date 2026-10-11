#pragma once

#include <string>

namespace hl {

/* Turns a game's executable (and window title as a fallback) into a readable name:
 * "VALORANT-Win64-Shipping.exe" -> "VALORANT", "cs2.exe" -> "Counter-Strike 2". */
std::string game_display_name(const std::string &executable, const std::string &window_title);

/* True for games Highlight knows by name. Used to capture them even when they run
 * in a window, which automatic (fullscreen) detection skips. Java only counts for a
 * game window: LWJGL's window class (Minecraft and its clients, other LWJGL games)
 * or a known Minecraft client title. */
bool is_known_game(const std::string &executable, const std::string &window_title,
		   const std::string &window_class);

/* java.exe / javaw.exe: runs every Java app, so never remembered as a game */
bool is_java_runtime(const std::string &executable);

} // namespace hl

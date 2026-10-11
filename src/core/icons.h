#pragma once

#include <windows.h>

namespace hl {

enum class IconState { Idle, Live, Saved, Paused, Recording };

/* Draws the Highlight mark for the tray / window. Requires GDI+ to be started. */
HICON make_icon(IconState state, int size);

} // namespace hl

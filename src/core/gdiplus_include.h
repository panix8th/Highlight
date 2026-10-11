#pragma once

/* GDI+ needs min/max, which NOMINMAX removes from windows.h. */
#include <windows.h>
#include <objidl.h>

#include <algorithm>

namespace Gdiplus {
using std::max;
using std::min;
} // namespace Gdiplus

#include <gdiplus.h>

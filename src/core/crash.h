#pragma once

/* Writes a line to the log and a small crash dump (logs\crash-*.dmp) when the
 * capture process dies, so a crash on someone's PC can be traced afterwards. */

namespace hl::crash {

void install();

} // namespace hl::crash

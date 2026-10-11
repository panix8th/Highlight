#pragma once

#include <cstdarg>

namespace hl::log {

enum Level { Debug = 0, Info = 1, Warning = 2, Error = 3 };

/* Opens %LOCALAPPDATA%\Highlight\logs\<name>.log. The previous log is kept as <name>.old.log. */
void init(const char *name);
void shutdown();

void write(Level level, const char *fmt, ...);
void writev(Level level, const char *fmt, va_list args);

} // namespace hl::log

#define HL_DEBUG(...) ::hl::log::write(::hl::log::Debug, __VA_ARGS__)
#define HL_INFO(...) ::hl::log::write(::hl::log::Info, __VA_ARGS__)
#define HL_WARN(...) ::hl::log::write(::hl::log::Warning, __VA_ARGS__)
#define HL_ERROR(...) ::hl::log::write(::hl::log::Error, __VA_ARGS__)

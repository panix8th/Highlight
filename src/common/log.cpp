#include "log.h"
#include "paths.h"

#include <windows.h>

#include <cstdio>
#include <mutex>

namespace hl::log {

static std::mutex g_mutex;
static FILE *g_file = nullptr;

void init(const char *name)
{
	std::lock_guard lock(g_mutex);
	if (g_file)
		return;

	auto dir = paths::log_dir();
	std::wstring base = std::wstring(name, name + strlen(name));
	auto current = dir / (base + L".log");
	auto old = dir / (base + L".old.log");

	std::error_code ec;
	if (std::filesystem::exists(current, ec))
		std::filesystem::rename(current, old, ec);

	g_file = _wfopen(current.c_str(), L"wb");
}

void shutdown()
{
	std::lock_guard lock(g_mutex);
	if (g_file) {
		fclose(g_file);
		g_file = nullptr;
	}
}

void writev(Level level, const char *fmt, va_list args)
{
	static const char *names[] = {"debug", "info", "warning", "error"};

	char msg[4096];
	vsnprintf(msg, sizeof(msg), fmt, args);

	SYSTEMTIME t;
	GetLocalTime(&t);
	char line[4200];
	snprintf(line, sizeof(line), "%02d:%02d:%02d.%03d [%s] %s\n", t.wHour, t.wMinute, t.wSecond, t.wMilliseconds,
		 names[level], msg);

	std::lock_guard lock(g_mutex);
	if (g_file) {
		fputs(line, g_file);
		fflush(g_file);
	}
#ifndef NDEBUG
	OutputDebugStringA(line);
#endif
}

void write(Level level, const char *fmt, ...)
{
	va_list args;
	va_start(args, fmt);
	writev(level, fmt, args);
	va_end(args);
}

} // namespace hl::log

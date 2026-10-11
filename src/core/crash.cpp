#include "crash.h"

#include "../common/log.h"
#include "../common/paths.h"

#include <windows.h>
#include <dbghelp.h>

#include <cstdio>
#include <cstdlib>
#include <exception>
#include <string>

namespace hl::crash {

namespace {

using MiniDumpWriteDumpFn = BOOL(WINAPI *)(HANDLE, DWORD, HANDLE, MINIDUMP_TYPE, PMINIDUMP_EXCEPTION_INFORMATION,
					   PMINIDUMP_USER_STREAM_INFORMATION, PMINIDUMP_CALLBACK_INFORMATION);

std::wstring g_dir;
LONG g_crashing = 0;

/* Small dump (threads, stacks, loaded modules): enough to see where it died. */
void write_dump(EXCEPTION_POINTERS *ep)
{
	HMODULE dbghelp = LoadLibraryW(L"dbghelp.dll");
	auto dump = dbghelp ? (MiniDumpWriteDumpFn)GetProcAddress(dbghelp, "MiniDumpWriteDump") : nullptr;
	if (!dump)
		return;
	SYSTEMTIME t;
	GetLocalTime(&t);
	wchar_t name[64];
	swprintf(name, 64, L"\\crash-%04d%02d%02d-%02d%02d%02d.dmp", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute,
		 t.wSecond);
	std::wstring path = g_dir + name;
	HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
	if (file == INVALID_HANDLE_VALUE)
		return;
	MINIDUMP_EXCEPTION_INFORMATION info = {};
	info.ThreadId = GetCurrentThreadId();
	info.ExceptionPointers = ep;
	info.ClientPointers = FALSE;
	dump(GetCurrentProcess(), GetCurrentProcessId(), file,
	     (MINIDUMP_TYPE)(MiniDumpWithThreadInfo | MiniDumpWithIndirectlyReferencedMemory), ep ? &info : nullptr,
	     nullptr, nullptr);
	CloseHandle(file);
	HL_ERROR("Crash dump written to %ls", path.c_str());
}

LONG WINAPI on_unhandled(EXCEPTION_POINTERS *ep)
{
	if (InterlockedExchange(&g_crashing, 1))
		return EXCEPTION_EXECUTE_HANDLER;
	const DWORD code = ep && ep->ExceptionRecord ? ep->ExceptionRecord->ExceptionCode : 0;
	const void *addr = ep && ep->ExceptionRecord ? ep->ExceptionRecord->ExceptionAddress : nullptr;
	HMODULE mod = nullptr;
	wchar_t mod_name[MAX_PATH] = L"?";
	if (addr && GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
				       (LPCWSTR)addr, &mod))
		GetModuleFileNameW(mod, mod_name, MAX_PATH);
	HL_ERROR("Crash: exception 0x%08lX at %p in %ls (offset 0x%llx)", code, addr, mod_name,
		 mod ? (unsigned long long)((const char *)addr - (const char *)mod) : 0ull);
	write_dump(ep);
	return EXCEPTION_EXECUTE_HANDLER;
}

void on_terminate()
{
	std::string what = "unknown";
	if (auto e = std::current_exception()) {
		try {
			std::rethrow_exception(e);
		} catch (const std::exception &ex) {
			what = ex.what();
		} catch (...) {
		}
	}
	if (!InterlockedExchange(&g_crashing, 1)) {
		HL_ERROR("Crash: unhandled C++ exception: %s", what.c_str());
		write_dump(nullptr);
	}
	std::abort();
}

} // namespace

void install()
{
	g_dir = paths::log_dir().wstring();
	SetUnhandledExceptionFilter(on_unhandled);
	std::set_terminate(on_terminate);
}

} // namespace hl::crash

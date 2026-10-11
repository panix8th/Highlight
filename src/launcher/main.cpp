/* Highlight.exe at the top of the install folder: starts the background core
 * (which opens the window). Exists so people do not have to dig into bin\64bit. */

#include <windows.h>

#include <string>

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int)
{
	wchar_t buf[MAX_PATH * 2];
	DWORD len = GetModuleFileNameW(nullptr, buf, (DWORD)(sizeof(buf) / sizeof(buf[0])));
	std::wstring dir(buf, len);
	dir = dir.substr(0, dir.find_last_of(L"\\/"));

	std::wstring bin = dir + L"\\bin\\64bit";
	std::wstring exe = bin + L"\\highlight.exe";
	std::wstring cmd = L"\"" + exe + L"\"";

	STARTUPINFOW si = {sizeof(si)};
	PROCESS_INFORMATION pi = {};
	if (!CreateProcessW(exe.c_str(), cmd.data(), nullptr, nullptr, FALSE, 0, nullptr, bin.c_str(), &si, &pi)) {
		MessageBoxW(nullptr, L"Could not find bin\\64bit\\highlight.exe next to Highlight.exe.", L"Highlight",
			    MB_ICONERROR);
		return 1;
	}
	CloseHandle(pi.hThread);
	CloseHandle(pi.hProcess);
	return 0;
}

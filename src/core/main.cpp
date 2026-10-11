/* highlight.exe - the background capture process. */

#include "app.h"

#include "../common/ipc.h"
#include "../common/log.h"
#include "../common/paths.h"

#include <windows.h>
#include <objbase.h>

#include <string>

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, PWSTR cmdline, int)
{
	const bool background = cmdline && wcsstr(cmdline, L"--background") != nullptr;

	SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

	/* One core per user session. A second launch just asks the first to show the UI. */
	HANDLE mutex = CreateMutexW(nullptr, TRUE, L"Local\\HighlightCore");
	if (GetLastError() == ERROR_ALREADY_EXISTS) {
		if (!background) {
			hl::ipc::Client client;
			if (client.connect(3000))
				client.send({{"cmd", "open_ui"}});
		}
		if (mutex)
			CloseHandle(mutex);
		return 0;
	}

	/* libobs resolves its data and plugins relative to the working directory. */
	SetCurrentDirectoryW(hl::paths::exe_dir().c_str());
	CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
	SetPriorityClass(GetCurrentProcess(), ABOVE_NORMAL_PRIORITY_CLASS);

	hl::log::init("core");
	HL_INFO("Highlight %s starting%s", HL_VERSION, background ? " in the background" : "");

	int code = 0;
	{
		hl::App app;
		code = app.run(inst, !background);
	}

	hl::log::shutdown();
	CoUninitialize();
	if (mutex) {
		ReleaseMutex(mutex);
		CloseHandle(mutex);
	}
	return code;
}

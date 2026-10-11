#include "paths.h"

#include <windows.h>
#include <shlobj.h>

namespace hl::paths {

static fs::path known_folder(REFKNOWNFOLDERID id)
{
	PWSTR raw = nullptr;
	fs::path out;
	if (SUCCEEDED(SHGetKnownFolderPath(id, KF_FLAG_CREATE, nullptr, &raw)))
		out = raw;
	CoTaskMemFree(raw);
	return out;
}

static fs::path ensure(const fs::path &p)
{
	std::error_code ec;
	fs::create_directories(p, ec);
	return p;
}

fs::path exe_dir()
{
	wchar_t buf[MAX_PATH * 2];
	DWORD len = GetModuleFileNameW(nullptr, buf, (DWORD)(sizeof(buf) / sizeof(buf[0])));
	return fs::path(std::wstring(buf, len)).parent_path();
}

fs::path install_dir()
{
	fs::path dir = exe_dir();
	/* bin\64bit\highlight.exe -> install root */
	if (dir.filename() == L"64bit" && dir.parent_path().filename() == L"bin")
		return dir.parent_path().parent_path();
	return dir;
}

fs::path app_data()
{
	return ensure(known_folder(FOLDERID_RoamingAppData) / L"Highlight");
}

fs::path local_data()
{
	return ensure(known_folder(FOLDERID_LocalAppData) / L"Highlight");
}

fs::path settings_file()
{
	return app_data() / L"settings.json";
}

fs::path library_file()
{
	return app_data() / L"library.json";
}

fs::path log_dir()
{
	return ensure(local_data() / L"logs");
}

fs::path thumbs_dir()
{
	return ensure(local_data() / L"thumbs");
}

fs::path default_clips_dir()
{
	fs::path videos = known_folder(FOLDERID_Videos);
	if (videos.empty())
		videos = known_folder(FOLDERID_Profile) / L"Videos";
	return videos / L"Highlight";
}

fs::path default_buffer_dir()
{
	return local_data() / L"ring";
}

fs::path fonts_dir()
{
	return install_dir() / L"data" / L"highlight" / L"fonts";
}

} // namespace hl::paths

#pragma once

#include <filesystem>

namespace hl::paths {

namespace fs = std::filesystem;

/* Folder that holds highlight.exe (bin\64bit inside the install). */
fs::path exe_dir();

/* Root of the install: the folder that contains bin\, data\ and Highlight.exe. */
fs::path install_dir();

/* %APPDATA%\Highlight - settings and library metadata. Created on demand. */
fs::path app_data();

/* %LOCALAPPDATA%\Highlight - logs, thumbnails, default rolling buffer. */
fs::path local_data();

fs::path settings_file();
fs::path library_file();
fs::path log_dir();
fs::path thumbs_dir();

/* %USERPROFILE%\Videos\Highlight */
fs::path default_clips_dir();

/* Default location of the rolling buffer when the user has not picked a drive. */
fs::path default_buffer_dir();

fs::path fonts_dir();

} // namespace hl::paths

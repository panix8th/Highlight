cmake_minimum_required(VERSION 3.24)

project(Highlight VERSION 1.0.3 DESCRIPTION "Lightweight game clipping for Windows" LANGUAGES C CXX RC)

if(NOT WIN32)
  message(FATAL_ERROR "Highlight is a Windows application. Build it on Windows (see README) or let GitHub Actions do it.")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

# Created by scripts/fetch-deps.ps1: the official OBS Studio runtime, the libobs
# headers from the same tag, obs-deps (FFmpeg, nlohmann/json) and an import
# library for obs.dll.
set(HL_DEPS_DIR "${CMAKE_SOURCE_DIR}/.deps" CACHE PATH "Dependency folder created by scripts/fetch-deps.ps1")
set(OBS_SOURCE_DIR "${HL_DEPS_DIR}/obs-src")
set(OBS_RUNTIME_DIR "${HL_DEPS_DIR}/obs-runtime")
set(OBS_DEPS_DIR "${HL_DEPS_DIR}/obs-deps")

foreach(required "${OBS_SOURCE_DIR}/libobs/obs.h" "${OBS_RUNTIME_DIR}/bin/64bit/obs.dll" "${OBS_DEPS_DIR}/include/libavformat/avformat.h")
  if(NOT EXISTS "${required}")
    message(FATAL_ERROR "Missing ${required}. Run scripts/fetch-deps.ps1 first.")
  endif()
endforeach()

# --- libobs (official obs.dll + generated import library) -------------------
add_library(libobs SHARED IMPORTED GLOBAL)
if(MSVC)
  set(OBS_IMPORT_LIB "${HL_DEPS_DIR}/obs.lib")
else()
  # MinGW links straight against the DLL (used for local compile checks only).
  set(OBS_IMPORT_LIB "${OBS_RUNTIME_DIR}/bin/64bit/obs.dll")
endif()
set_target_properties(libobs PROPERTIES
  IMPORTED_IMPLIB "${OBS_IMPORT_LIB}"
  IMPORTED_LOCATION "${OBS_RUNTIME_DIR}/bin/64bit/obs.dll"
  INTERFACE_INCLUDE_DIRECTORIES "${OBS_SOURCE_DIR}/libobs;${CMAKE_SOURCE_DIR}/cmake/obsconfig")

# --- FFmpeg from obs-deps (same build OBS ships) ----------------------------
add_library(ffmpeg INTERFACE)
target_include_directories(ffmpeg SYSTEM INTERFACE "${OBS_DEPS_DIR}/include")
foreach(lib avformat avcodec avfilter avutil swscale swresample)
  if(MSVC)
    target_link_libraries(ffmpeg INTERFACE "${OBS_DEPS_DIR}/lib/${lib}.lib")
  else()
    file(GLOB _dll "${OBS_DEPS_DIR}/bin/${lib}-*.dll")
    target_link_libraries(ffmpeg INTERFACE "${_dll}")
  endif()
endforeach()

# --- common compile settings -------------------------------------------------
add_library(hl_options INTERFACE)
target_compile_definitions(hl_options INTERFACE
  UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN _WIN32_WINNT=0x0A00 WINVER=0x0A00
  PSAPI_VERSION=2 HL_VERSION="${PROJECT_VERSION}")
if(MSVC)
  target_compile_options(hl_options INTERFACE /W3 /utf-8 /permissive- /Zc:__cplusplus /EHsc /MP)
  target_compile_definitions(hl_options INTERFACE _CRT_SECURE_NO_WARNINGS)
else()
  target_compile_options(hl_options INTERFACE -Wall -Wno-unknown-pragmas -Wno-missing-field-initializers)
  target_link_options(hl_options INTERFACE -municode)
endif()

function(hl_add_resources target description name)
  set(HL_ICON_PATH "${CMAKE_SOURCE_DIR}/assets/icons/highlight.ico")
  set(HL_RC_DESCRIPTION "${description}")
  set(HL_RC_NAME "${name}")
  configure_file("${CMAKE_SOURCE_DIR}/cmake/version.rc.in" "${CMAKE_BINARY_DIR}/res/${target}.rc" @ONLY)
  target_sources(${target} PRIVATE "${CMAKE_BINARY_DIR}/res/${target}.rc")
endfunction()

# --- shared code -------------------------------------------------------------
add_library(hl_common STATIC
  src/common/games.cpp
  src/common/image.cpp
  src/common/ipc.cpp
  src/common/library.cpp
  src/common/logo.cpp
  src/common/log.cpp
  src/common/paths.cpp
  src/common/settings.cpp
  src/common/strings.cpp)
target_include_directories(hl_common PUBLIC src)
target_include_directories(hl_common SYSTEM PUBLIC "${OBS_DEPS_DIR}/include")
target_link_libraries(hl_common PUBLIC hl_options shell32 ole32 windowscodecs crypt32)

add_library(hl_media STATIC
  src/media/audio_mix.cpp
  src/media/av_util.cpp
  src/media/mp4_writer.cpp
  src/media/probe.cpp
  src/media/remux.cpp
  src/media/share.cpp
  src/media/thumbnail.cpp
  src/media/waveform.cpp
  src/media/player.cpp)
target_link_libraries(hl_media PUBLIC hl_common ffmpeg)

# Rolling buffer and clip writer: no libobs, so tests can drive them directly.
add_library(hl_clip STATIC
  src/core/clip_writer.cpp
  src/core/disk_ring.cpp)
target_link_libraries(hl_clip PUBLIC hl_media)

# --- highlight.exe: background capture core ---------------------------------
add_executable(highlight WIN32
  src/core/main.cpp
  src/core/app.cpp
  src/core/crash.cpp
  src/core/engine.cpp
  src/core/hotkeys.cpp
  src/core/icons.cpp
  src/core/ring_output.cpp
  src/core/sound.cpp
  src/core/toast.cpp)
target_link_libraries(highlight PRIVATE hl_clip libobs gdiplus winmm psapi dwmapi user32 gdi32 advapi32)
hl_add_resources(highlight "Highlight capture" highlight)

# --- highlight-ui.exe: the window, started on demand ------------------------
add_library(imgui STATIC
  third_party/imgui/imgui.cpp
  third_party/imgui/imgui_draw.cpp
  third_party/imgui/imgui_tables.cpp
  third_party/imgui/imgui_widgets.cpp
  third_party/imgui/backends/imgui_impl_win32.cpp
  third_party/imgui/backends/imgui_impl_dx11.cpp)
target_include_directories(imgui PUBLIC third_party/imgui third_party/imgui/backends)
target_compile_definitions(imgui PUBLIC IMGUI_DISABLE_OBSOLETE_FUNCTIONS UNICODE _UNICODE NOMINMAX)
if(MSVC)
  target_compile_options(imgui PRIVATE /W0 /utf-8)
endif()

add_executable(highlight-ui WIN32
  src/ui/main.cpp
  src/ui/app_ui.cpp
  src/ui/theme.cpp
  src/ui/widgets.cpp
  src/ui/library_page.cpp
  src/ui/editor_page.cpp
  src/ui/settings_pages.cpp
  src/ui/share_dialog.cpp
  src/ui/textures.cpp
  src/ui/links.cpp
  src/ui/r2.cpp
  src/ui/updater.cpp
  src/ui/audio_out.cpp)
target_link_libraries(highlight-ui PRIVATE hl_media imgui winhttp bcrypt d3d11 dxgi d3dcompiler dwmapi winmm shell32 ole32 oleaut32 uuid comdlg32 user32 gdi32)
hl_add_resources(highlight-ui "Highlight" highlight-ui)

# --- Highlight.exe: tiny launcher at the top of the install folder ----------
add_executable(launcher WIN32 src/launcher/main.cpp)
# Own folder: "Highlight.exe" and "highlight.exe" are the same name on Windows.
set_target_properties(launcher PROPERTIES OUTPUT_NAME Highlight RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/launcher")
target_link_libraries(launcher PRIVATE hl_options)
hl_add_resources(launcher "Highlight launcher" Highlight)

# --- tests (off by default; CI turns them on) --------------------------------
option(HL_BUILD_TESTS "Build the test programs" OFF)
if(HL_BUILD_TESTS)
  add_executable(clip_pipeline_test tests/clip_pipeline_test.cpp)
  target_link_libraries(clip_pipeline_test PRIVATE hl_clip)
endif()

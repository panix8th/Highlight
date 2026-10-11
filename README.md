# Highlight

**Hit the key. Keep the moment.**

Highlight is a lightweight game clipping app for Windows, built on the OBS Studio capture engine. It quietly holds the last minute (or five) of your game and saves it straight to your PC when you press a key. Nothing is uploaded.

## Download

Every push builds the app on GitHub Actions:

1. Open the **Actions** tab, pick the latest green **Build Windows app** run.
2. Download the **Highlight-windows-x64** artifact and unzip it anywhere.
3. Run **Highlight.exe**.

Tagged versions (`v0.1.0`, …) are also published as a zip on the **Releases** page.

Requirements: Windows 10 2004 or newer (per-app audio capture needs it), a DirectX 11 GPU. NVIDIA (NVENC) is the main target; AMD, Intel and CPU (x264) encoders are used automatically as fallbacks.

## What it does

**Clipping**
- Rolling buffer of the last 15 s – 5 min (or any custom length), plus optional "keep recording after the press" post-roll.
- Separate hotkeys for a normal clip (F8), a long clip (Shift+F8), a quick clip (Alt+F8), and up to four extra save keys with their own lengths.
- Bookmarks (F9) that show up as markers on the clip timeline, screenshots (F10), full-session recording (Ctrl+F9), pause/resume (Ctrl+Shift+F8).
- Game capture with auto-detection (starts when a game runs, stops on the desktop), or whole-screen / single-window capture.
- In-game pop-up and sound when a clip is saved, REC indicator, tray icon with all actions.

**Quality**
- Presets from Lightweight (1080p30) to Ultra (native, 120 fps); resolution, 30–240 fps, NVENC HEVC / AV1 / H.264, constant quality / VBR / CBR, keyframe interval, NVENC preset, look-ahead, psycho-visual tuning, HDR, MP4 / MKV / MOV.

**Audio**
- Up to five tracks: the game's own sound, any output device, microphones, single apps (Discord, Spotify…).
- "Mixed + separate" layout: track 1 plays anywhere, the other tracks stay apart for editing.
- Noise suppression (RNNoise), noise gate, push-to-talk for clips, mute mic in clips, AAC or Opus, 128–320 kbps.

**Library & editor**
- Thumbnail grid grouped by day, filters per game, favourites, sessions, screenshots, search and sort.
- Editor with preview playback, frame stepping, trim handles, per-track mute, waveforms and bookmark markers.
- Lossless trim (instant, no quality loss) or frame-exact GPU re-encode. Save a copy or replace the original.

**Share copies (for Discord and friends)**
- Only when you share: your original clip always stays full size and full quality.
- Fit under 10 MB / 50 MB / 500 MB / a custom size: Highlight picks the best resolution and frame rate for the limit and encodes on your GPU with quality-focused NVENC settings (retries automatically if a file overshoots).
- Or "Best quality": a constant-quality copy that looks the same as the original but is several times smaller than the recording.
- H.264 (plays everywhere), HEVC or AV1. The finished file is put on the clipboard: press Ctrl+V in Discord.

## Why it stays light

- **The buffer lives on your SSD, not in RAM.** OBS's built-in replay buffer keeps the whole clip in memory (about 300 MB for one minute at high quality, 1.5 GB for five). Highlight streams the encoded video into short segment files and deletes old ones as it goes. Saving a clip just copies the right segments into an MP4, with no re-encode. Memory stays flat whatever the clip length.
- **Two processes.** `highlight.exe` (capture, encoder, hotkeys, tray) runs in the background. The window (`highlight-ui.exe`) only exists while it is open; closing it frees its memory.
- **GPU encoding** (NVENC) keeps the CPU free for your game.
- Only the OBS pieces Highlight needs are shipped and loaded: game/display/window capture, WASAPI audio, NVENC/AMF/QSV/x264 encoders, the AAC/Opus encoders and the noise filters.

## Folder layout

```
Highlight\
  Highlight.exe              starts everything
  bin\64bit\                 highlight.exe (capture), highlight-ui.exe (window), obs.dll, FFmpeg
  data\, obs-plugins\        OBS shaders and plugins
```

Settings: `%APPDATA%\Highlight\settings.json` · Logs: `%LOCALAPPDATA%\Highlight\logs` · Clips: `Videos\Highlight` by default.

## Building locally

GitHub Actions does all of this for you (`.github/workflows/build.yml`). To build on your own PC:

1. Install Visual Studio 2022 (Desktop development with C++), CMake 3.24+, Ninja and Git.
2. Open a **Developer PowerShell for VS 2022** in the repo and run `./scripts/fetch-deps.ps1`. It downloads the official OBS Studio 32.2.2 runtime, the libobs headers from the same tag, OBS's prebuilt FFmpeg (obs-deps) and generates an import library for `obs.dll`.
3. `cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release`
4. `cmake --build build`
5. `./scripts/package.ps1` assembles `dist\Highlight` and a zip.

## Notes and limits

- The in-game pop-up shows over borderless and windowed games. Exclusive-fullscreen games hide it, but the sound still plays.
- Game capture uses OBS's signed capture hook, which most anti-cheat systems accept. If a game blocks it, switch to whole-screen capture.
- Automatic clipping of kills and wins is planned; it needs game-specific integrations.

## Licence

Highlight is free software under the GNU General Public License v2.0 or later (see `LICENSE`), because it links libobs (GPL). It bundles OBS Studio components and FFmpeg (GPL builds from obs-deps), Dear ImGui (MIT), and the Archivo and JetBrains Mono fonts (SIL Open Font License).

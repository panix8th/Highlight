<#
  Assembles a portable Highlight folder (and a zip of it) from the build output
  and the parts of the OBS runtime Highlight actually uses.

  Layout (mirrors OBS so libobs finds its data with relative paths):
    Highlight\
      Highlight.exe                 launcher
      bin\64bit\                    highlight.exe, highlight-ui.exe, obs.dll, FFmpeg ...
      data\libobs\                  shaders
      data\obs-plugins\<plugin>\    plugin data (game capture hook, locales)
      data\highlight\fonts\
      obs-plugins\64bit\            the few plugins we load
      licenses\
#>
param(
    [string]$BuildDir = "build",
    [string]$OutDir = "dist",
    [string]$Deps = (Join-Path $PSScriptRoot "..\.deps"),
    [string]$Version = ""
)

$ErrorActionPreference = "Stop"
$Root = Resolve-Path (Join-Path $PSScriptRoot "..")
$Deps = (Resolve-Path $Deps).Path
$runtime = Join-Path $Deps "obs-runtime"

if (-not $Version) {
    $cm = Get-Content (Join-Path $Root "CMakeLists.txt") -Raw
    if ($cm -match 'project\(Highlight VERSION ([0-9.]+)') { $Version = $Matches[1] } else { $Version = "0.0.0" }
}

New-Item -ItemType Directory -Force -Path $OutDir | Out-Null
$app = Join-Path (Resolve-Path $OutDir) "Highlight"
if (Test-Path $app) { Remove-Item -Recurse -Force $app }
$bin = Join-Path $app "bin\64bit"
New-Item -ItemType Directory -Force -Path $bin | Out-Null

function Copy-Required([string]$From, [string]$To) {
    if (-not (Test-Path $From)) { throw "Missing $From" }
    Copy-Item $From $To -Force
}

# Highlight binaries
Copy-Required (Join-Path $BuildDir "highlight.exe") $bin
Copy-Required (Join-Path $BuildDir "highlight-ui.exe") $bin
Copy-Required (Join-Path $BuildDir "launcher\Highlight.exe") $app

# libobs + FFmpeg + helpers from the OBS runtime
$obsBin = Join-Path $runtime "bin\64bit"
$patterns = @("obs.dll", "w32-pthreads.dll", "libobs-d3d11.dll", "libobs-winrt.dll", "zlib.dll",
    "libcurl.dll", "librist.dll", "srt.dll", "libx264-*.dll",
    "avcodec-*.dll", "avformat-*.dll", "avfilter-*.dll", "avdevice-*.dll", "avutil-*.dll",
    "swresample-*.dll", "swscale-*.dll",
    "obs-ffmpeg-mux.exe", "obs-nvenc-test.exe", "obs-amf-test.exe", "obs-qsv-test.exe")
foreach ($p in $patterns) {
    $found = Get-ChildItem -Path $obsBin -Filter $p
    if (-not $found) { throw "OBS runtime is missing $p" }
    $found | ForEach-Object { Copy-Item $_.FullName $bin -Force }
}

# libobs shaders
Copy-Item (Join-Path $runtime "data\libobs") (Join-Path $app "data\libobs") -Recurse -Force

# Only the plugins Highlight loads
$plugins = @("win-capture", "win-wasapi", "obs-nvenc", "obs-ffmpeg", "obs-filters", "obs-x264", "obs-qsv11")
$pluginBin = Join-Path $app "obs-plugins\64bit"
New-Item -ItemType Directory -Force -Path $pluginBin | Out-Null
foreach ($p in $plugins) {
    Copy-Required (Join-Path $runtime "obs-plugins\64bit\$p.dll") $pluginBin
    $data = Join-Path $runtime "data\obs-plugins\$p"
    if (Test-Path $data) {
        $target = Join-Path $app "data\obs-plugins\$p"
        Copy-Item $data $target -Recurse -Force
        Get-ChildItem -Path $target -Recurse -Filter *.pdb | Remove-Item -Force
    }
}

# Fonts
$fonts = Join-Path $app "data\highlight\fonts"
New-Item -ItemType Directory -Force -Path $fonts | Out-Null
Copy-Item (Join-Path $Root "assets\fonts\*.ttf") $fonts -Force

# Microsoft C++ runtime next to the exes, so a fresh Windows install works.
if ($env:VCToolsRedistDir) {
    $crt = Get-ChildItem -Path (Join-Path $env:VCToolsRedistDir "x64") -Directory -Filter "Microsoft.VC*.CRT" | Select-Object -First 1
    if ($crt) {
        Get-ChildItem $crt.FullName -Filter *.dll | ForEach-Object { Copy-Item $_.FullName $bin -Force }
        Write-Host "Bundled C++ runtime from $($crt.FullName)"
    }
}

# Licenses
$lic = Join-Path $app "licenses"
New-Item -ItemType Directory -Force -Path $lic | Out-Null
Copy-Item (Join-Path $Root "LICENSE") (Join-Path $lic "Highlight-GPL-2.0.txt")
Copy-Item (Join-Path $Root "assets\fonts\OFL-*.txt") $lic
Copy-Item (Join-Path $Root "third_party\imgui\LICENSE.txt") (Join-Path $lic "imgui-MIT.txt")
@"
Highlight bundles parts of OBS Studio $((Get-Item (Join-Path $obsBin 'obs.dll')).VersionInfo.ProductVersion)
(libobs and plugins, GPL-2.0-or-later, https://github.com/obsproject/obs-studio)
and FFmpeg (GPL build from obs-deps, https://github.com/obsproject/obs-deps).
Source for Highlight: https://github.com/panix8th/clippingpanix
"@ | Set-Content -Encoding utf8 (Join-Path $lic "THIRD-PARTY.txt")
Copy-Item (Join-Path $Root "README.md") $app -ErrorAction SilentlyContinue

# Zip
$zip = Join-Path (Resolve-Path $OutDir) "Highlight-$Version-windows-x64.zip"
if (Test-Path $zip) { Remove-Item $zip }
Compress-Archive -Path $app -DestinationPath $zip -CompressionLevel Optimal
$size = [math]::Round((Get-Item $zip).Length / 1MB, 1)
Write-Host "Packaged $app"
Write-Host "Zip: $zip ($size MB)"

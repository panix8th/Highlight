<#
  Downloads everything Highlight builds against into .deps\:
    obs-runtime\  the official OBS Studio portable build (obs.dll, plugins, data)
    obs-src\      libobs headers from the same OBS tag
    obs-deps\     OBS's prebuilt dependencies (FFmpeg headers + import libs, nlohmann/json)
    obs.lib       import library generated from obs.dll

  Run from a "Developer PowerShell for VS 2022" (needs dumpbin.exe and lib.exe).
#>
param(
    [string]$ObsVersion = "32.2.2",
    [string]$ObsSha256 = "4d6e40e3ab155f56b30de517380566a206d74b63cdf5ad49aa596924768f97e1",
    [string]$DepsVersion = "2026-07-15",
    [string]$DepsSha256 = "6f90e9598fa10cff5ad23cdcfae49b87868c07bf896b02cd464582b4ce2f2ba9",
    [string]$Dest = (Join-Path $PSScriptRoot "..\.deps")
)

$ErrorActionPreference = "Stop"
$ProgressPreference = "SilentlyContinue"

New-Item -ItemType Directory -Force -Path $Dest | Out-Null
$Dest = (Resolve-Path $Dest).Path
Write-Host "Dependencies go to $Dest"

function Get-Verified([string]$Url, [string]$OutFile, [string]$Sha256) {
    if (Test-Path $OutFile) {
        $have = (Get-FileHash $OutFile -Algorithm SHA256).Hash.ToLower()
        if ($have -eq $Sha256) { Write-Host "  cached: $(Split-Path $OutFile -Leaf)"; return }
        Remove-Item $OutFile
    }
    Write-Host "  downloading $Url"
    Invoke-WebRequest -Uri $Url -OutFile $OutFile -UseBasicParsing
    $got = (Get-FileHash $OutFile -Algorithm SHA256).Hash.ToLower()
    if ($got -ne $Sha256) { throw "SHA-256 mismatch for $OutFile (got $got)" }
}

function Expand-Into([string]$Zip, [string]$Dir) {
    if (Test-Path $Dir) { Remove-Item -Recurse -Force $Dir }
    New-Item -ItemType Directory -Force -Path $Dir | Out-Null
    # tar (bsdtar) ships with Windows 10+ and is much faster than Expand-Archive
    tar -xf $Zip -C $Dir
    if ($LASTEXITCODE -ne 0) { throw "Could not extract $Zip" }
}

# 1. OBS Studio runtime --------------------------------------------------------
$runtime = Join-Path $Dest "obs-runtime"
if (-not (Test-Path (Join-Path $runtime "bin\64bit\obs.dll"))) {
    $zip = Join-Path $Dest "OBS-Studio-$ObsVersion-Windows-x64.zip"
    Get-Verified "https://github.com/obsproject/obs-studio/releases/download/$ObsVersion/OBS-Studio-$ObsVersion-Windows-x64.zip" $zip $ObsSha256
    Expand-Into $zip $runtime
}

# 2. obs-deps (FFmpeg dev files) ----------------------------------------------
$deps = Join-Path $Dest "obs-deps"
if (-not (Test-Path (Join-Path $deps "include\libavformat\avformat.h"))) {
    $zip = Join-Path $Dest "windows-deps-$DepsVersion-x64.zip"
    Get-Verified "https://github.com/obsproject/obs-deps/releases/download/$DepsVersion/windows-deps-$DepsVersion-x64.zip" $zip $DepsSha256
    Expand-Into $zip $deps
}

# 3. libobs headers from the matching tag -------------------------------------
$src = Join-Path $Dest "obs-src"
if (-not (Test-Path (Join-Path $src "libobs\obs.h"))) {
    if (Test-Path $src) { Remove-Item -Recurse -Force $src }
    git clone --depth 1 --branch $ObsVersion --filter=blob:none --sparse https://github.com/obsproject/obs-studio.git $src
    if ($LASTEXITCODE -ne 0) { throw "git clone failed" }
    git -C $src sparse-checkout set libobs
    if ($LASTEXITCODE -ne 0) { throw "git sparse-checkout failed" }
}

# 4. Import library for obs.dll ------------------------------------------------
$lib = Join-Path $Dest "obs.lib"
if (-not (Test-Path $lib)) {
    $dll = Join-Path $runtime "bin\64bit\obs.dll"
    $def = Join-Path $Dest "obs.def"
    $exports = & dumpbin.exe /nologo /exports $dll
    if ($LASTEXITCODE -ne 0) { throw "dumpbin failed (run from a VS developer shell)" }
    $names = foreach ($line in $exports) {
        if ($line -match '^\s+\d+\s+[0-9A-Fa-f]+\s+[0-9A-Fa-f]{8}\s+([A-Za-z_][A-Za-z0-9_@?$]*)') { $Matches[1] }
    }
    if ($names.Count -lt 500) { throw "Only found $($names.Count) exports in obs.dll" }
    @("LIBRARY obs", "EXPORTS") + $names | Set-Content -Encoding ascii $def
    & lib.exe /nologo /def:$def /out:$lib /machine:x64
    if ($LASTEXITCODE -ne 0) { throw "lib.exe failed" }
    Write-Host "  generated obs.lib with $($names.Count) exports"
}

Write-Host "Dependencies ready."

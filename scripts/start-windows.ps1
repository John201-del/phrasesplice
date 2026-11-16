# SPDX-License-Identifier: GPL-3.0-only
$ErrorActionPreference = 'Stop'
$repository = [IO.Path]::GetFullPath((Split-Path -Parent $PSScriptRoot))
function Resolve-LocalDirectory([string]$Path) {
    if ($Path -notmatch '^[A-Za-z]:[\\/]' -or ($Path -split '[\\/]') -contains '..') {
        throw 'Use an absolute local directory path without parent traversal.'
    }
    if ($Path -match '[^\x20-\x7e]') { throw 'Use ASCII paths for the runtime and data folders in this preview (MLT limitation).' }
    $full = [IO.Path]::GetFullPath($Path)
    for ($ancestor = $full; $ancestor; $ancestor = [IO.Path]::GetDirectoryName($ancestor)) {
        $item = Get-Item -LiteralPath $ancestor -Force -ErrorAction SilentlyContinue
        if ($item) {
            if (-not $item.PSIsContainer -or ($item.Attributes -band [IO.FileAttributes]::ReparsePoint)) {
                throw 'Runtime and data paths must use ordinary directories, not files, junctions or symbolic links.'
            }
        }
    }
    return $full
}
$runtime = Resolve-LocalDirectory (Join-Path $repository 'runtime')
$dataRoot = if ($env:LOCALCUT_WORKSPACE_ROOT) { $env:LOCALCUT_WORKSPACE_ROOT } else { Join-Path $runtime 'userdata' }
$dataRoot = Resolve-LocalDirectory $dataRoot
foreach ($relative in @('tmp', 'cache/qml', 'cache/shaders')) {
    Resolve-LocalDirectory (Join-Path $dataRoot $relative) | Out-Null
}
if (-not (Test-Path -LiteralPath "$runtime/localcut-release.json")) {
    & "$PSScriptRoot/install-windows.ps1"
}
& "$PSScriptRoot/check-runtime.ps1" -Runtime $runtime
$env:LOCALCUT_WORKSPACE_ROOT = $dataRoot
$env:PATH = "$runtime/bin;$env:SystemRoot/System32;$env:SystemRoot"
$env:QT_PLUGIN_PATH = "$runtime/bin;$runtime/lib/plugins"
$env:QML_IMPORT_PATH = "$runtime/lib/qml;$runtime/bin"
$env:QML2_IMPORT_PATH = $env:QML_IMPORT_PATH
$env:QT_QUICK_CONTROLS_STYLE = 'org.kde.desktop'
$env:QT_QPA_PLATFORM = 'windows'
$env:LANGUAGE = 'zh_CN'
$env:LC_ALL = 'en_US.UTF-8'
$env:MLT_DATA = "$runtime/share/mlt"
$env:MLT_PROFILES_PATH = "$runtime/share/mlt/profiles"
$env:MLT_PRESETS_PATH = "$runtime/share/mlt/presets"
$env:LADSPA_PATH = "$runtime/lib/ladspa"
$env:FREI0R_PATH = "$runtime/lib/frei0r-1"
foreach ($name in @('MLT_APPDIR','MLT_PREFIX','MLT_REPOSITORY')) {
    [Environment]::SetEnvironmentVariable($name, $null, 'Process')
}
$env:TEMP = "$env:LOCALCUT_WORKSPACE_ROOT/tmp"
$env:TMP = $env:TEMP
$env:QML_DISK_CACHE_PATH = "$env:LOCALCUT_WORKSPACE_ROOT/cache/qml"
$env:QT_SHADER_CACHE_PATH = "$env:LOCALCUT_WORKSPACE_ROOT/cache/shaders"
$env:PYTHONUTF8 = '1'
$env:PYTHONDONTWRITEBYTECODE = '1'
[IO.Directory]::CreateDirectory($env:TEMP) | Out-Null
Start-Process -FilePath "$runtime/bin/kdenlive.exe" -ArgumentList '--no-welcome' -WorkingDirectory $runtime -WindowStyle Normal

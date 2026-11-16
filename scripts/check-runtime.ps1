# SPDX-License-Identifier: GPL-3.0-only
param([string]$Runtime = (Join-Path (Split-Path -Parent $PSScriptRoot) 'runtime'))
$ErrorActionPreference = 'Stop'
$Runtime = [IO.Path]::GetFullPath($Runtime)
foreach ($relative in @('bin/kdenlive.exe','bin/kdenlive_render.exe','bin/localcut-mcp.exe','bin/Qt6Core.dll','bin/melt.exe',
    'bin/ffmpeg.exe','bin/ffprobe.exe','bin/platforms/qwindows.dll','share/mlt/profiles',
    'bin/data/kdenlive/assistant/edit_templates.json','bin/data/kdenlive/scripts/local_ai/process_media.py','localcut-release.json')) {
    if (-not (Test-Path -LiteralPath (Join-Path $Runtime $relative))) { throw "Runtime file missing: $relative. Reinstall the complete runtime." }
}
$stamp = Get-Content -LiteralPath "$Runtime/localcut-release.json" -Raw -Encoding UTF8 | ConvertFrom-Json
$manifest = Get-Content -LiteralPath (Join-Path (Split-Path -Parent $PSScriptRoot) 'distribution/windows.json') -Raw -Encoding UTF8 | ConvertFrom-Json
if ($stamp.version -ne $manifest.version) {
    throw 'The installed application version differs from this source checkout. Keep your userdata and install this version in a new directory.'
}
if ($stamp.files -isnot [array] -or $stamp.files.Count -eq 0) {
    throw 'Application hash manifest must contain a nonempty files array.'
}
$paths = [Collections.Generic.HashSet[string]]::new([StringComparer]::OrdinalIgnoreCase)
$prefix = $Runtime.TrimEnd([char[]]'\/') + [IO.Path]::DirectorySeparatorChar
foreach ($file in $stamp.files) {
    if ($file.path -isnot [string] -or -not $file.path -or $file.sha256 -isnot [string] -or $file.sha256 -notmatch '^[0-9a-fA-F]{64}$') {
        throw 'Invalid application hash entry.'
    }
    $relative = $file.path.Replace('\', '/')
    if ([IO.Path]::IsPathRooted($relative) -or $relative.Contains(':') -or ($relative -split '/') -contains '..' -or
        -not [IO.Path]::GetFullPath((Join-Path $Runtime $relative)).StartsWith($prefix, [StringComparison]::OrdinalIgnoreCase)) {
        throw 'Application hash path must stay inside the runtime.'
    }
    if (-not $paths.Add($relative)) { throw ('Duplicate application hash path: ' + $relative) }
}
foreach ($required in @('bin/kdenlive.exe','bin/kdenlive_render.exe','bin/localcut-mcp.exe',
    'bin/data/kdenlive/assistant/edit_templates.json','bin/data/kdenlive/scripts/local_ai/process_media.py')) {
    if (-not $paths.Contains($required)) { throw ('Required application hash entry is missing: ' + $required) }
}
if (-not (Test-Path -LiteralPath "$Runtime/bin/ogg.dll") -or
    (Get-FileHash -LiteralPath "$Runtime/bin/ogg.dll" -Algorithm SHA256).Hash -ne 'e122a485d3377c455495bace62d49b6b14736cd3afc298b9d2f07303eee98687') {
    throw 'The verified Ogg compatibility DLL required by the SoX audio plugin is missing or changed.'
}
foreach ($file in $stamp.files) {
    $target = Join-Path $Runtime $file.path
    if (-not (Test-Path -LiteralPath $target -PathType Leaf) -or (Get-FileHash -LiteralPath $target -Algorithm SHA256).Hash -ne $file.sha256) {
        throw ('Application file changed or missing: ' + $file.path)
    }
}
$previousPath = $env:PATH
try {
    $env:PATH = "$Runtime/bin;$env:SystemRoot/System32;$env:SystemRoot"
    $version = & "$Runtime/bin/ffmpeg.exe" -version 2>&1
    if ($LASTEXITCODE -ne 0) { throw 'FFmpeg could not start. The runtime may be incomplete.' }
    $configuration = $version -join "`n"
    if ($configuration -match '--enable-nonfree' -or $configuration -notmatch '--enable-gpl') { throw 'Unexpected FFmpeg license configuration.' }
    Write-Output ("PhraseSplice $($stamp.version): application hashes verified; GPL FFmpeg available.")
} finally { $env:PATH = $previousPath }

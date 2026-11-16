# SPDX-License-Identifier: GPL-3.0-only
# Third-party binaries are downloaded from their publishers, not redistributed here.
param([string]$OverlayArchive)
$ErrorActionPreference = 'Stop'
$repository = [IO.Path]::GetFullPath((Split-Path -Parent $PSScriptRoot))
if (-not [Environment]::Is64BitOperatingSystem) { throw 'PhraseSplice requires 64-bit Windows.' }
if ($repository -match '[^\x20-\x7e]') { throw 'Use an English/ASCII folder path for this preview (MLT runtime limitation).' }
for ($ancestor = $repository; $ancestor; $ancestor = [IO.Path]::GetDirectoryName($ancestor)) {
    if ((Get-Item -LiteralPath $ancestor -Force).Attributes -band [IO.FileAttributes]::ReparsePoint) {
        throw 'Install in a normal local folder, not a junction or symbolic link.'
    }
}
$manifest = Get-Content -LiteralPath "$repository/distribution/windows.json" -Raw -Encoding UTF8 | ConvertFrom-Json
if (-not $manifest.overlay.sha256) { throw 'This source snapshot has no published Windows application build yet. See docs/BUILD.md.' }
$runtime = Join-Path $repository 'runtime'
if (Test-Path -LiteralPath "$runtime/localcut-release.json") {
    & "$PSScriptRoot/check-runtime.ps1" -Runtime $runtime
    return
}
if (Test-Path -LiteralPath $runtime) { throw 'An incomplete runtime folder exists. Keep your userdata, rename runtime, then retry.' }
$cache = Join-Path $repository '.downloads'
$stage = Join-Path $repository ('.install-' + $manifest.version + '-' + [Guid]::NewGuid().ToString('N'))
foreach ($path in @($cache, $stage)) {
    if ((Test-Path -LiteralPath $path) -and ((Get-Item -LiteralPath $path -Force).Attributes -band [IO.FileAttributes]::ReparsePoint)) {
        throw 'Download/staging folders must not be links.'
    }
    [IO.Directory]::CreateDirectory($path) | Out-Null
}
[Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12
function Verified-Download($Entry) {
    $file = Join-Path $cache $Entry.file
    if (-not (Test-Path -LiteralPath $file)) {
        Write-Host ('Downloading ' + $Entry.file)
        & curl.exe --fail --location --retry 3 --connect-timeout 20 --max-time 900 --output ($file + '.part') $Entry.url
        if ($LASTEXITCODE -ne 0) { throw ('Download failed: ' + $Entry.file + '. Retry when the connection is available.') }
        if ((Get-FileHash -LiteralPath ($file + '.part') -Algorithm SHA256).Hash -ne $Entry.sha256) { throw ('SHA-256 mismatch: ' + $Entry.file) }
        Move-Item -LiteralPath ($file + '.part') -Destination $file
    }
    if ((Get-FileHash -LiteralPath $file -Algorithm SHA256).Hash -ne $Entry.sha256) { throw ('Cached SHA-256 mismatch: ' + $file) }
    return $file
}
$extractor = Verified-Download $manifest.extractor
$kdeArchive = Verified-Download $manifest.kdenlive
$ffmpegArchive = Verified-Download $manifest.ffmpeg
if ($OverlayArchive) {
    $overlay = (Resolve-Path -LiteralPath $OverlayArchive).Path
    if ((Get-FileHash -LiteralPath $overlay -Algorithm SHA256).Hash -ne $manifest.overlay.sha256) { throw 'Application overlay SHA-256 mismatch.' }
} else { $overlay = Verified-Download $manifest.overlay }
Write-Output 'Extracting verified runtime dependencies...'
$exclusions = @('-xr!ffmpeg.exe','-xr!ffprobe.exe','-xr!ffplay.exe','-xr!avcodec-*.dll','-xr!avdevice-*.dll',
    '-xr!avfilter-*.dll','-xr!avformat-*.dll','-xr!avutil-*.dll','-xr!swresample-*.dll','-xr!swscale-*.dll',
    '-xr!kdenlive.exe','-xr!kdenlive_render.exe')
& $extractor x -y "-o$stage/kde" $kdeArchive @exclusions | Out-Host
if ($LASTEXITCODE -ne 0) { throw 'KDE runtime extraction failed.' }
Expand-Archive -LiteralPath $ffmpegArchive -DestinationPath "$stage/ffmpeg" -Force
$base = Join-Path $stage 'kde/kdenlive-26.08.3_standalone'
$ffmpeg = @(Get-ChildItem -LiteralPath "$stage/ffmpeg" -Directory)
if ($ffmpeg.Count -ne 1 -or -not (Test-Path -LiteralPath "$base/bin/Qt6Core.dll")) { throw 'Unexpected dependency archive layout.' }
Copy-Item -Path "$($ffmpeg[0].FullName)/bin/*" -Destination "$base/bin" -Force
# The pinned KDE package's libsox-3.dll imports ogg.dll but ships it as libogg.dll.
$oggHash = 'e122a485d3377c455495bace62d49b6b14736cd3afc298b9d2f07303eee98687'
if ((Get-FileHash -LiteralPath "$base/bin/libogg.dll" -Algorithm SHA256).Hash -ne $oggHash) { throw 'Unexpected Ogg compatibility library.' }
Copy-Item -LiteralPath "$base/bin/libogg.dll" -Destination "$base/bin/ogg.dll"
Expand-Archive -LiteralPath $overlay -DestinationPath $base -Force
& "$PSScriptRoot/check-runtime.ps1" -Runtime $base
# Both paths were computed beneath the checked checkout; never replace existing user data.
if (-not [IO.Path]::GetFullPath($base).StartsWith($stage + [IO.Path]::DirectorySeparatorChar, [StringComparison]::OrdinalIgnoreCase)) {
    throw 'Unexpected staging path.'
}
if (@(Get-ChildItem -LiteralPath $stage -Recurse -Force | Where-Object { $_.Attributes -band [IO.FileAttributes]::ReparsePoint }).Count) {
    throw 'Unexpected link in extracted runtime. Installation stopped before activation.'
}
Move-Item -LiteralPath $base -Destination $runtime
$stagingItems = @(Get-ChildItem -LiteralPath $stage -Recurse -Force)
if (@($stagingItems | Where-Object { $_.Attributes -band [IO.FileAttributes]::ReparsePoint }).Count) {
    throw 'Unexpected link in staging folder; installed runtime kept and staging cleanup stopped.'
}
foreach ($item in @($stagingItems | Sort-Object { $_.FullName.Length } -Descending)) { Remove-Item -LiteralPath $item.FullName -Force }
Remove-Item -LiteralPath $stage -Force
Write-Output 'PhraseSplice is installed. Start-PhraseSplice.cmd opens the editor. No models or account credentials were installed.'

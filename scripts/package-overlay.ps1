#Requires -Version 7.0
# SPDX-License-Identifier: GPL-3.0-only
$ErrorActionPreference = 'Stop'
$repository = Split-Path -Parent $PSScriptRoot
$installed = Join-Path $repository 'build/installed'
$manifestPath = Join-Path $repository 'distribution/windows.json'
$manifest = Get-Content -LiteralPath $manifestPath -Raw -Encoding UTF8 | ConvertFrom-Json
$version = $manifest.version
$destination = Join-Path $repository ('dist/overlay-' + [Guid]::NewGuid().ToString('N'))
$archive = Join-Path $repository "dist/phrasesplice-windows-x64-$version.zip"
if (Test-Path -LiteralPath $archive) { throw 'An overlay archive already exists. Preserve or remove that exact previous artifact before repackaging.' }
foreach ($required in @('bin/kdenlive.exe','bin/kdenlive_render.exe','bin/localcut-mcp.exe','bin/data/kdenlive/assistant/edit_templates.json')) {
    if (-not (Test-Path -LiteralPath (Join-Path $installed $required))) { throw "Build/install this checkout first. Missing $required" }
}
$items = @(Get-ChildItem -LiteralPath $installed -Recurse -Force)
if (@($items | Where-Object { $_.Attributes -band [IO.FileAttributes]::ReparsePoint }).Count) { throw 'Installed components contain a link.' }
[IO.Directory]::CreateDirectory($destination) | Out-Null
$files = @()
foreach ($item in $items) {
    if ($item.PSIsContainer -or $item.Name -match '\.(a|cmake|pdb|debug|h)$') { continue }
    $relative = [IO.Path]::GetRelativePath($installed, $item.FullName).Replace('\','/')
    if ($relative -match '^bin/data/doc/HTML/[^/]+/kdenlive/') { continue }
    $target = Join-Path $destination $relative
    [IO.Directory]::CreateDirectory([IO.Path]::GetDirectoryName($target)) | Out-Null
    [IO.File]::Copy($item.FullName, $target, $false)
    $files += @{ path = $relative; sha256 = (Get-FileHash -LiteralPath $target -Algorithm SHA256).Hash.ToLowerInvariant() }
}
Copy-Item -LiteralPath "$repository/COPYING" -Destination "$destination/LOCALCUT-COPYING"
Copy-Item -LiteralPath "$repository/AUTHORS" -Destination "$destination/KDENLIVE-AUTHORS"
$stamp = @{ version = $version; source = "https://github.com/John201-del/phrasesplice/tree/v$version"; files = $files }
$stamp | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath "$destination/localcut-release.json" -Encoding utf8
Compress-Archive -Path "$destination/*" -DestinationPath $archive -CompressionLevel Optimal
$overlay = @{ file = [IO.Path]::GetFileName($archive); sha256 = (Get-FileHash -LiteralPath $archive -Algorithm SHA256).Hash.ToLowerInvariant();
    url = "https://github.com/John201-del/phrasesplice/releases/download/v$version/$([IO.Path]::GetFileName($archive))" }
$manifest | Add-Member -MemberType NoteProperty -Name overlay -Value $overlay -Force
$manifest | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath $manifestPath -Encoding utf8
Write-Output "Application overlay: $archive"
Write-Output ('SHA-256: ' + $overlay.sha256)
Write-Output 'distribution/windows.json now describes this local application build. Validate before publishing.'

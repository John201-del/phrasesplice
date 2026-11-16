# SPDX-License-Identifier: GPL-3.0-only
# Negative contracts only: placeholder executables must never be invoked.
param([Parameter(Mandatory)][string]$OutputDirectory)
$ErrorActionPreference = 'Stop'
$repository = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '../..'))
if ($OutputDirectory -notmatch '^[A-Za-z]:[\\/]') { throw 'Use an absolute local test directory.' }
$OutputDirectory = [IO.Path]::GetFullPath($OutputDirectory)
if (Test-Path -LiteralPath $OutputDirectory) {
    throw 'Use a new empty test directory.'
}
$fixture = Join-Path $OutputDirectory 'fixture'
[IO.Directory]::CreateDirectory($fixture) | Out-Null
$version = (Get-Content -LiteralPath "$repository/distribution/windows.json" -Raw -Encoding UTF8 | ConvertFrom-Json).version
$required = @('bin/kdenlive.exe','bin/kdenlive_render.exe','bin/localcut-mcp.exe','bin/data/kdenlive/assistant/edit_templates.json','bin/data/kdenlive/scripts/local_ai/process_media.py')
$emptySha = 'e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855'
foreach ($relative in $required + @('bin/Qt6Core.dll','bin/melt.exe','bin/ffmpeg.exe','bin/ffprobe.exe','bin/platforms/qwindows.dll')) {
    $path = Join-Path $fixture $relative
    [IO.Directory]::CreateDirectory([IO.Path]::GetDirectoryName($path)) | Out-Null
    [IO.File]::WriteAllBytes($path, [byte[]]@())
}
[IO.Directory]::CreateDirectory((Join-Path $fixture 'share/mlt/profiles')) | Out-Null
# Deliberately omit ogg.dll: even an unfixed validator stops before any fake EXE.
function Entries { return @($required | ForEach-Object { @{ path = $_; sha256 = $emptySha } }) }
$cases = @(
    @{ name = 'missing-files'; stamp = @{ version = $version }; expected = 'nonempty files array' },
    @{ name = 'empty-files'; stamp = @{ version = $version; files = @() }; expected = 'nonempty files array' },
    @{ name = 'wrong-version'; stamp = @{ version = 'wrong-version'; files = @(Entries) }; expected = 'version differs' }
)
foreach ($missing in $required) {
    $cases += @{ name = "missing-$missing"; stamp = @{ version = $version; files = @((Entries) | Where-Object { $_.path -ne $missing }) }; expected = 'Required application hash entry is missing' }
}
foreach ($badSha in @('', 'abc', ('g' * 64))) {
    $entries = @(Entries)
    $entries[0].sha256 = $badSha
    $cases += @{ name = "invalid-sha-$($badSha.Length)"; stamp = @{ version = $version; files = $entries }; expected = 'Invalid application hash entry' }
}
foreach ($badPath in @('C:\outside.exe','/outside.exe','\\server\share\outside.exe','../outside.exe','bin/../../outside.exe')) {
    $entries = @(Entries) + @{ path = $badPath; sha256 = $emptySha }
    $cases += @{ name = "invalid-path-$badPath"; stamp = @{ version = $version; files = $entries }; expected = 'Application hash path must stay inside the runtime' }
}
$cases += @{ name = 'duplicate-path'; stamp = @{ version = $version; files = @((Entries)) + @{ path = 'BIN/KDENLIVE.EXE'; sha256 = $emptySha } }; expected = 'Duplicate application hash path' }
$results = @()
try {
    foreach ($case in $cases) {
        [IO.File]::WriteAllText("$fixture/localcut-release.json", ($case.stamp | ConvertTo-Json -Depth 5), [Text.UTF8Encoding]::new($false))
        $message = ''
        try { & "$repository/scripts/check-runtime.ps1" -Runtime $fixture | Out-Null } catch { $message = $_.Exception.Message }
        $passed = $message.Contains($case.expected)
        $results += @{ name = $case.name; passed = $passed; message = $message }
        $label = if ($passed) { 'PASS ' } else { 'FAIL ' }
        Write-Output ($label + $case.name)
    }
} finally {
    [IO.File]::WriteAllText("$OutputDirectory/report.json", ($results | ConvertTo-Json -Depth 4), [Text.UTF8Encoding]::new($false))
    $resolved = [IO.Path]::GetFullPath($fixture)
    if ($resolved -ne (Join-Path $OutputDirectory 'fixture') -or -not $resolved.StartsWith($OutputDirectory + '\', [StringComparison]::OrdinalIgnoreCase)) { throw 'Unsafe fixture cleanup path' }
    Remove-Item -LiteralPath $fixture -Recurse -Force
}
$failed = @($results | Where-Object { -not $_.passed }).Count
if ($failed) { throw "$failed of $($results.Count) runtime rejection contracts failed. See $OutputDirectory/report.json" }
Write-Output "$($results.Count) rejection contracts passed; no native EXE was executed."

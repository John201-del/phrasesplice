# SPDX-License-Identifier: GPL-3.0-only
# Real MLT decode/render smoke test; this does not validate the editor UI or assistant.
param(
    [string]$Runtime = (Join-Path (Split-Path -Parent $PSScriptRoot) 'runtime'),
    [string]$OutputDirectory = (Join-Path (Split-Path -Parent $PSScriptRoot) 'build/smoke')
)
$ErrorActionPreference = 'Stop'
$Runtime = [IO.Path]::GetFullPath($Runtime)
$OutputDirectory = [IO.Path]::GetFullPath($OutputDirectory)
foreach ($path in @($Runtime, $OutputDirectory)) {
    if ($path -match '[^\x20-\x7e]') { throw 'Use ASCII runtime and smoke-test paths for the MLT preview.' }
    for ($ancestor = $path; $ancestor; $ancestor = [IO.Path]::GetDirectoryName($ancestor)) {
        if ((Test-Path -LiteralPath $ancestor) -and ((Get-Item -LiteralPath $ancestor -Force).Attributes -band [IO.FileAttributes]::ReparsePoint)) {
            throw 'Runtime and output paths must not use links or junctions.'
        }
    }
}
if ((Test-Path -LiteralPath $OutputDirectory) -and @(Get-ChildItem -LiteralPath $OutputDirectory -Force).Count) {
    throw 'Choose a new or empty OutputDirectory; smoke tests never reuse previous outputs.'
}
[IO.Directory]::CreateDirectory($OutputDirectory) | Out-Null
$report = [ordered]@{
    schemaVersion = 1; ok = $false; scope = 'runtime_decode_render_only'
    startedUtc = [DateTime]::UtcNow.ToString('o'); runtime = $Runtime; outputDirectory = $OutputDirectory
    expected = @{ width = 320; height = 180; frames = 50; fps = '25/1'; seconds = 2; videoCodec = 'h264'; audioCodec = 'aac' }
    nativeRuns = @(); source = $null; rendered = $null; error = $null
}
$environment = @{
    PATH = "$Runtime/bin;$env:SystemRoot/System32;$env:SystemRoot"
    TEMP = "$OutputDirectory/tmp"; TMP = "$OutputDirectory/tmp"; TMPDIR = "$OutputDirectory/tmp"
    LOCALCUT_WORKSPACE_ROOT = "$OutputDirectory/userdata"
    XDG_CACHE_HOME = "$OutputDirectory/cache"; XDG_CONFIG_HOME = "$OutputDirectory/config"; XDG_DATA_HOME = "$OutputDirectory/data"
    QML_DISK_CACHE_PATH = "$OutputDirectory/cache/qml"; QT_SHADER_CACHE_PATH = "$OutputDirectory/cache/shaders"
    QT_PLUGIN_PATH = "$Runtime/bin;$Runtime/lib/plugins"; QT_QPA_PLATFORM = 'offscreen'
    MLT_DATA = "$Runtime/share/mlt"; MLT_PROFILES_PATH = "$Runtime/share/mlt/profiles"; MLT_PRESETS_PATH = "$Runtime/share/mlt/presets"
    MLT_APPDIR = $null; MLT_PREFIX = $null; MLT_REPOSITORY = $null; MLT_NO_VDPAU = '1'
    LADSPA_PATH = "$Runtime/lib/ladspa"; FREI0R_PATH = "$Runtime/lib/frei0r-1"
    SDL_AUDIODRIVER = 'dummy'; SDL_VIDEODRIVER = 'dummy'
}
$previousEnvironment = @{}
function Invoke-Native([string]$Name, [string]$Executable, [string[]]$Arguments) {
    $stdout = Join-Path $OutputDirectory "$Name.stdout.log"
    $stderr = Join-Path $OutputDirectory "$Name.stderr.log"
    $argumentLine = (@($Arguments | ForEach-Object { '"' + $_ + '"' }) -join ' ')
    $process = Start-Process -FilePath $Executable -ArgumentList $argumentLine -WorkingDirectory $OutputDirectory `
        -WindowStyle Hidden -RedirectStandardOutput $stdout -RedirectStandardError $stderr -PassThru
    $process.Handle | Out-Null
    if (-not $process.WaitForExit(120000)) {
        $process.Kill()
        $process.WaitForExit()
        throw "$Name exceeded the two-minute smoke-test limit. See $stderr"
    }
    $process.WaitForExit()
    $exitCode = $process.ExitCode
    $report.nativeRuns += @{ name = $Name; executable = $Executable; exitCode = $exitCode; stdout = $stdout; stderr = $stderr }
    if ($null -eq $exitCode -or $exitCode -ne 0) { throw "$Name failed with exit code $exitCode. See $stderr" }
}
function Read-Media([string]$Path, [string]$Name) {
    Invoke-Native $Name "$Runtime/bin/ffprobe.exe" @('-v', 'error', '-count_frames', '-show_streams', '-show_format', '-of', 'json', $Path)
    if ((Get-Item -LiteralPath "$OutputDirectory/$Name.stderr.log").Length) { throw "$Name reported a media decoding error." }
    $media = Get-Content -LiteralPath "$OutputDirectory/$Name.stdout.log" -Raw -Encoding UTF8 | ConvertFrom-Json
    $video = @($media.streams | Where-Object codec_type -eq 'video')
    $audio = @($media.streams | Where-Object codec_type -eq 'audio')
    if ($video.Count -ne 1 -or $audio.Count -ne 1) { throw "$Name must contain exactly one video and one audio stream." }
    $seconds = [double]::Parse($media.format.duration, [Globalization.CultureInfo]::InvariantCulture)
    $audioSeconds = [double]::Parse($audio[0].duration, [Globalization.CultureInfo]::InvariantCulture)
    if ($video[0].codec_name -ne 'h264' -or $video[0].width -ne 320 -or $video[0].height -ne 180 `
        -or $video[0].avg_frame_rate -ne '25/1' -or [int]$video[0].nb_read_frames -ne 50 `
        -or $audio[0].codec_name -ne 'aac' -or [Math]::Abs($seconds - 2) -gt 0.10 -or [Math]::Abs($audioSeconds - 2) -gt 0.10) {
        throw "$Name failed the dimensions, frame count, frame rate, duration or codec checks."
    }
    return $media
}
try {
    foreach ($name in $environment.Keys) {
        $previousEnvironment[$name] = [Environment]::GetEnvironmentVariable($name, 'Process')
        [Environment]::SetEnvironmentVariable($name, $environment[$name], 'Process')
    }
    foreach ($relative in @('tmp', 'userdata', 'cache/qml', 'cache/shaders', 'config', 'data')) {
        [IO.Directory]::CreateDirectory((Join-Path $OutputDirectory $relative)) | Out-Null
    }
    foreach ($relative in @('bin/melt.exe', 'bin/ffmpeg.exe', 'bin/ffprobe.exe', 'share/mlt/profiles')) {
        if (-not (Test-Path -LiteralPath (Join-Path $Runtime $relative))) { throw "Runtime file missing: $relative" }
    }
    $source = Join-Path $OutputDirectory 'source.mp4'
    $rendered = Join-Path $OutputDirectory 'rendered.mp4'
    $profile = Join-Path $OutputDirectory 'smoke-profile'
    [IO.File]::WriteAllText($profile, @'
description=PhraseSplice finite smoke profile
frame_rate_num=25
frame_rate_den=1
width=320
height=180
progressive=1
sample_aspect_num=1
sample_aspect_den=1
display_aspect_num=16
display_aspect_den=9
colorspace=709
'@, [Text.UTF8Encoding]::new($false))
    Invoke-Native 'generate-source' "$Runtime/bin/ffmpeg.exe" @('-nostdin', '-v', 'error', '-xerror', '-y',
        '-f', 'lavfi', '-i', 'testsrc2=size=320x180:rate=25:duration=2',
        '-f', 'lavfi', '-i', 'sine=frequency=440:sample_rate=48000:duration=2',
        '-map', '0:v:0', '-map', '1:a:0', '-t', '2', '-c:v', 'libx264', '-preset', 'ultrafast', '-pix_fmt', 'yuv420p',
        '-c:a', 'aac', '-b:a', '128k', '-threads', '2', $source)
    $report.source = Read-Media $source 'probe-source'
    # Two finite one-second playlist entries exercise native source trimming and composition.
    Invoke-Native 'mlt-render' "$Runtime/bin/melt.exe" @('-profile', $profile, '-progress',
        $source, 'in=25', 'out=49', $source, 'in=0', 'out=24',
        '-consumer', "avformat:$rendered", 'f=mp4', 'vcodec=libx264', 'acodec=aac', 'pix_fmt=yuv420p',
        'preset=ultrafast', 'crf=23', 'ab=128k', 'ar=48000', 'threads=2', 'real_time=-1')
    $report.rendered = Read-Media $rendered 'probe-rendered'
    # Reject a silent/blank fallback even when a container has valid metadata.
    Invoke-Native 'check-video-signal' "$Runtime/bin/ffmpeg.exe" @('-nostdin', '-v', 'info', '-xerror', '-i', $rendered,
        '-map', '0:v:0', '-vf', 'signalstats,metadata=print', '-frames:v', '1', '-an', '-f', 'null', '-')
    $signal = Get-Content -LiteralPath "$OutputDirectory/check-video-signal.stderr.log" -Raw
    $minimum = [regex]::Match($signal, 'lavfi.signalstats.YMIN=([0-9.]+)')
    $maximum = [regex]::Match($signal, 'lavfi.signalstats.YMAX=([0-9.]+)')
    if (-not $minimum.Success -or -not $maximum.Success -or ([double]$maximum.Groups[1].Value - [double]$minimum.Groups[1].Value) -lt 60) {
        throw 'The rendered video lacks the expected test-pattern luminance range.'
    }
    Invoke-Native 'check-audio-signal' "$Runtime/bin/ffmpeg.exe" @('-nostdin', '-v', 'info', '-xerror', '-i', $rendered,
        '-map', '0:a:0', '-af', 'volumedetect', '-vn', '-f', 'null', '-')
    $volume = [regex]::Match((Get-Content -LiteralPath "$OutputDirectory/check-audio-signal.stderr.log" -Raw), 'mean_volume:\s*(-?[0-9.]+) dB')
    if (-not $volume.Success -or [double]$volume.Groups[1].Value -lt -45 -or [double]$volume.Groups[1].Value -gt -3) {
        throw 'The rendered audio lacks the expected sine-wave signal.'
    }
    $report.ok = $true
    Write-Output 'Runtime smoke passed: real MLT decoding, playlist rendering, H.264 video and AAC audio. Editor UI and assistant were not tested.'
} catch {
    $report.error = $_.Exception.Message
    throw
} finally {
    $report.finishedUtc = [DateTime]::UtcNow.ToString('o')
    [IO.File]::WriteAllText((Join-Path $OutputDirectory 'report.json'), ($report | ConvertTo-Json -Depth 10), [Text.UTF8Encoding]::new($false))
    foreach ($name in $previousEnvironment.Keys) { [Environment]::SetEnvironmentVariable($name, $previousEnvironment[$name], 'Process') }
}

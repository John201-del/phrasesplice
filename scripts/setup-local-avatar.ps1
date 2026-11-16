param([string]$RuntimeRoot)
$ErrorActionPreference = 'Stop'
$project = Split-Path -Parent $PSScriptRoot
$adapter = Join-Path $project 'data\scripts\avatar'
$dataRoot = if ($env:LOCALCUT_WORKSPACE_ROOT) { $env:LOCALCUT_WORKSPACE_ROOT } else { Join-Path $project 'runtime\userdata' }
if ($dataRoot -notmatch '^(?:[A-Za-z]:[\\/]|\\\\)') { throw 'LOCALCUT_WORKSPACE_ROOT must be an absolute directory' }
$dataRoot = [IO.Path]::GetFullPath($dataRoot)
if (-not $RuntimeRoot) { $RuntimeRoot = Join-Path $dataRoot 'local-avatar' }
if ($RuntimeRoot -notmatch '^(?:[A-Za-z]:[\\/]|\\\\)') { throw 'Runtime must be an absolute directory' }
$resolved = [IO.Path]::GetFullPath($RuntimeRoot)
$prefix = $dataRoot.TrimEnd([char[]]'\/') + [IO.Path]::DirectorySeparatorChar
if (-not $resolved.StartsWith($prefix, [StringComparison]::OrdinalIgnoreCase)) { throw 'Runtime must be inside LOCALCUT_WORKSPACE_ROOT' }
for ($probe = $resolved; $probe; $probe = Split-Path -Parent $probe) {
    if ((Test-Path -LiteralPath $probe) -and ((Get-Item -LiteralPath $probe -Force).Attributes -band [IO.FileAttributes]::ReparsePoint)) { throw 'Runtime must not use reparse points' }
}
if ((Test-Path -LiteralPath $resolved) -and (Get-ChildItem -LiteralPath $resolved -Recurse -Force -Attributes ReparsePoint | Select-Object -First 1)) {
    throw 'Runtime must not contain reparse points'
}
$env:LOCALCUT_WORKSPACE_ROOT = $dataRoot
$base = Join-Path $dataRoot 'local-ai\python'
$pythonRoot = Join-Path $resolved 'python'
if (-not (Test-Path -LiteralPath "$pythonRoot\python.exe") -and -not (Test-Path -LiteralPath "$base\python.exe")) {
    throw 'Run setup-local-ai.ps1 with the same LOCALCUT_WORKSPACE_ROOT before installing the optional avatar runtime'
}
for ($probe = $base; $probe; $probe = Split-Path -Parent $probe) {
    if ((Test-Path -LiteralPath $probe) -and ((Get-Item -LiteralPath $probe -Force).Attributes -band [IO.FileAttributes]::ReparsePoint)) { throw 'Base Python must not use reparse points' }
}
if ((Test-Path -LiteralPath $base) -and (Get-ChildItem -LiteralPath $base -Recurse -Force -Attributes ReparsePoint | Select-Object -First 1)) {
    throw 'Base Python must not contain reparse points'
}
foreach ($name in @('python','tmp','cache','models','source','jobs')) { New-Item -ItemType Directory -Force -Path (Join-Path $resolved $name) | Out-Null }
if (-not (Test-Path -LiteralPath "$pythonRoot\python.exe")) {
    Get-ChildItem -LiteralPath $base -File | Copy-Item -Destination $pythonRoot
    New-Item -ItemType Directory -Force -Path "$pythonRoot\Lib\site-packages" | Out-Null
    Copy-Item -LiteralPath "$base\Lib\site-packages\pip" -Destination "$pythonRoot\Lib\site-packages" -Recurse
}
[IO.File]::WriteAllText("$pythonRoot\python311._pth", "python311.zip`n.`nLib/site-packages`n", [Text.UTF8Encoding]::new($false))
$python = Join-Path $pythonRoot 'python.exe'
$env:TEMP = Join-Path $resolved 'tmp'
$env:TMP = $env:TEMP
$env:PYTHONUTF8 = '1'
$env:PYTHONIOENCODING = 'utf-8'
$env:PYTHONNOUSERSITE = '1'
$env:PIP_CONFIG_FILE = 'NUL'
$env:MPLCONFIGDIR = Join-Path $resolved 'cache\matplotlib'
$env:MPLBACKEND = 'Agg'
$env:JOBLIB_TEMP_FOLDER = Join-Path $resolved 'cache\tmp'
$env:NUMBA_CACHE_DIR = Join-Path $resolved 'cache\numba'
$editorFfmpeg = Join-Path $project 'runtime\bin\ffmpeg.exe'
if (Test-Path -LiteralPath $editorFfmpeg -PathType Leaf) { $env:IMAGEIO_FFMPEG_EXE = $editorFfmpeg }
foreach ($directory in @($env:MPLCONFIGDIR,$env:JOBLIB_TEMP_FOLDER,$env:NUMBA_CACHE_DIR)) { New-Item -ItemType Directory -Force -Path $directory | Out-Null }
function Run-Python([string[]]$Arguments) {
    & $python @Arguments
    if ($LASTEXITCODE -ne 0) { throw 'Avatar setup step failed' }
}
Run-Python @("$adapter\download_file.py", 'https://download.pytorch.org/whl/cpu/torch-2.2.2%2Bcpu-cp311-cp311-win_amd64.whl', "$resolved\torch-2.2.2+cpu-cp311-cp311-win_amd64.whl", '200798025', '88e63c916e3275fa30a220ee736423a95573b96072ded85e5c0171fd8f37a755')
Run-Python @("$adapter\download_file.py", 'https://download.pytorch.org/whl/cpu/torchvision-0.17.2%2Bcpu-cp311-cp311-win_amd64.whl', "$resolved\torchvision-0.17.2+cpu-cp311-cp311-win_amd64.whl", '1158980', '54ae4b89038065e7393c65bc8ff141d1bf3c2f70f88badc834247666608ba9f4')
Run-Python @('-m', 'pip', 'install', '--no-deps', '--no-index', "$resolved\torch-2.2.2+cpu-cp311-cp311-win_amd64.whl", "$resolved\torchvision-0.17.2+cpu-cp311-cp311-win_amd64.whl")
Run-Python @("$adapter\download_dependencies.py", $resolved)
Run-Python @('-m', 'pip', 'install', '--no-cache-dir', '--no-index', '--find-links', "$resolved\wheels", '--require-hashes', '-r', "$adapter\requirements.lock")
$source = Join-Path $resolved 'source\SadTalker'
if (-not (Test-Path -LiteralPath "$source\.git")) {
    & git clone --filter=blob:none --no-checkout https://github.com/OpenTalker/SadTalker.git $source
    if ($LASTEXITCODE) { throw 'Source clone failed' }
}
if (-not (Test-Path -LiteralPath "$source\src\facerender\animate.py")) {
    & git -C $source sparse-checkout set src
    if ($LASTEXITCODE) { throw 'Source sparse checkout failed' }
    & git -C $source checkout cd4c0465ae0b54a6f85af57f5c65fec9fe23e7f8
    if ($LASTEXITCODE) { throw 'Source checkout failed' }
}
$revision = & git -C $source rev-parse HEAD
if ($revision -ne 'cd4c0465ae0b54a6f85af57f5c65fec9fe23e7f8') { throw 'Unexpected source revision' }
Run-Python @("$adapter\patch_source.py", $source)
Run-Python @("$adapter\check_imports.py", $resolved)
Run-Python @('-m', 'pip', 'check')
# Model transfer starts only after every inference import and CPU NMS have passed.
Run-Python @("$adapter\install_models.py", $resolved)
Write-Output "Avatar runtime ready: $python"

param([string]$Root)
$ErrorActionPreference = 'Stop'
$project = Split-Path -Parent $PSScriptRoot
$dataRoot = if ($env:LOCALCUT_WORKSPACE_ROOT) { $env:LOCALCUT_WORKSPACE_ROOT } else { Join-Path $project 'runtime\userdata' }
if ($dataRoot -notmatch '^(?:[A-Za-z]:[\\/]|\\\\)') { throw 'LOCALCUT_WORKSPACE_ROOT must be an absolute directory' }
$dataRoot = [IO.Path]::GetFullPath($dataRoot)
if (-not $Root) { $Root = Join-Path $dataRoot 'local-ai' }
if ($Root -notmatch '^(?:[A-Za-z]:[\\/]|\\\\)') { throw 'Runtime must be an absolute directory' }
$Root = [IO.Path]::GetFullPath($Root)
$prefix = $dataRoot.TrimEnd([char[]]'\/') + [IO.Path]::DirectorySeparatorChar
if (-not $Root.StartsWith($prefix, [StringComparison]::OrdinalIgnoreCase)) { throw 'Runtime must be inside LOCALCUT_WORKSPACE_ROOT' }
for ($probe = $Root; $probe; $probe = Split-Path -Parent $probe) {
    if ((Test-Path -LiteralPath $probe) -and ((Get-Item -LiteralPath $probe -Force).Attributes -band [IO.FileAttributes]::ReparsePoint)) { throw 'Runtime must not use reparse points' }
}
if ((Test-Path -LiteralPath $Root) -and (Get-ChildItem -LiteralPath $Root -Recurse -Force -Attributes ReparsePoint | Select-Object -First 1)) {
    throw 'Runtime must not contain reparse points'
}
$env:LOCALCUT_WORKSPACE_ROOT = $dataRoot
foreach ($name in @('tmp','cache','models','python')) { New-Item -ItemType Directory -Force -Path (Join-Path $Root $name) | Out-Null }
$env:TEMP = Join-Path $Root 'tmp'
$env:TMP = $env:TEMP
$env:PIP_CACHE_DIR = Join-Path $Root 'cache'
$env:PYTHONNOUSERSITE = '1'
$env:PYTHONUTF8 = '1'
$env:KERAS_HOME = Join-Path $Root 'cache\keras'
$env:XDG_CACHE_HOME = Join-Path $Root 'cache'
$env:TORCH_HOME = Join-Path $Root 'cache\torch'
$env:TF_NUM_INTRAOP_THREADS = '2'
$env:TF_NUM_INTEROP_THREADS = '1'
$env:CUDA_VISIBLE_DEVICES = '-1'
function Download([string]$Uri, [string]$Target) {
    if (-not (Test-Path -LiteralPath $Target)) {
        $part = $Target + '.part'
        for ($attempt = 0; $attempt -lt 10; $attempt++) {
            & curl.exe -L --fail --connect-timeout 20 --max-time 180 -C - --silent --show-error $Uri -o $part
            if ($LASTEXITCODE -eq 0) { break }
        }
        if ($LASTEXITCODE -ne 0) { throw "Download failed: $Target (partial retained for resume)" }
        Move-Item -LiteralPath $part -Destination $Target
    }
}
$Python = Join-Path $Root 'python\python.exe'
if (-not (Test-Path -LiteralPath $Python)) {
    Download 'https://www.python.org/ftp/python/3.11.9/python-3.11.9-embed-amd64.zip' (Join-Path $Root 'python311.zip')
    Expand-Archive -LiteralPath (Join-Path $Root 'python311.zip') -DestinationPath (Join-Path $Root 'python') -Force
}
Set-Content -LiteralPath (Join-Path $Root 'python\python311._pth') -Value "python311.zip`n.`nLib\site-packages`nimport site" -Encoding ascii
& $Python -m pip --version 2>$null
if ($LASTEXITCODE -ne 0) {
    Download 'https://bootstrap.pypa.io/get-pip.py' (Join-Path $Root 'get-pip.py')
    & $Python (Join-Path $Root 'get-pip.py') --index-url https://pypi.org/simple --no-warn-script-location
    if ($LASTEXITCODE -ne 0) { throw 'pip bootstrap failed' }
}
# Upstream pins tensorflow-io 0.32 without a Windows wheel. Local file inference
# uses no GCS; 0.31 is the Windows wheel compatible with TensorFlow 2.12.
& $Python -m pip install --index-url https://pypi.org/simple --no-deps --no-warn-script-location 'spleeter==2.4.2'
if ($LASTEXITCODE -ne 0) { throw 'Spleeter installation failed' }
& $Python -m pip install --index-url https://pypi.org/simple --only-binary=:all: --no-warn-script-location 'tensorflow==2.12.1' 'numpy==1.24.3' 'onnxruntime==1.20.1' 'opencv-python-headless==4.11.0.86' 'ffmpeg-python==0.2.0' 'httpx[http2]==0.19.0' 'typer==0.3.2' 'pandas==1.5.3' 'norbert==0.2.1' 'tensorflow-io-gcs-filesystem==0.31.0' 'grpcio==1.62.3' 'jax==0.4.30' 'jaxlib==0.4.30' 'scipy==1.11.4' 'setuptools==68.2.2'
if ($LASTEXITCODE -ne 0) { throw 'Dependency installation failed' }
Download 'https://github.com/PeterL1n/RobustVideoMatting/releases/download/v1.0.0/rvm_mobilenetv3_fp32.onnx' (Join-Path $Root 'models\rvm_mobilenetv3_fp32.onnx')
if ((Get-FileHash -LiteralPath (Join-Path $Root 'models\rvm_mobilenetv3_fp32.onnx')).Hash -ne '88D4531297118F595BF2FD60F6F566AEC2E559393802D1F436C380F0CBBD2828') { throw 'RVM SHA256 mismatch' }
Download 'https://github.com/deezer/spleeter/releases/download/v1.4.0/2stems.tar.gz' (Join-Path $Root 'models\2stems.tar.gz')
if ((Get-FileHash -LiteralPath (Join-Path $Root 'models\2stems.tar.gz')).Hash -ne 'F3A90B39DD2874269E8B05A48A86745DF897B848C61F3958EFC80A39152BD692') { throw 'Spleeter SHA256 mismatch' }
New-Item -ItemType Directory -Force -Path (Join-Path $Root 'models\2stems') | Out-Null
$missing = @('checkpoint','model.index','model.data-00000-of-00001') | Where-Object { -not (Test-Path -LiteralPath (Join-Path $Root "models\2stems\$_")) }
if ($missing) {
    & tar -xzf (Join-Path $Root 'models\2stems.tar.gz') -C (Join-Path $Root 'models\2stems')
    if ($LASTEXITCODE -ne 0) { throw 'Model extraction failed' }
}
Download 'https://raw.githubusercontent.com/PeterL1n/RobustVideoMatting/master/LICENSE' (Join-Path $Root 'models\RVM-LICENSE.txt')
Download 'https://raw.githubusercontent.com/deezer/spleeter/master/LICENSE' (Join-Path $Root 'models\Spleeter-LICENSE.txt')
Get-FileHash -LiteralPath (Join-Path $Root 'models\rvm_mobilenetv3_fp32.onnx'),(Join-Path $Root 'models\2stems.tar.gz') -Algorithm SHA256 | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $Root 'models\sha256.json') -Encoding utf8
& $Python -c 'import tensorflow,onnxruntime,cv2,numpy; from spleeter.separator import Separator; print("Local AI imports OK")'
if ($LASTEXITCODE -ne 0) { throw 'Runtime import check failed' }

$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'
if (-not $env:LOCALCUT_WORKSPACE_ROOT) { throw 'Set LOCALCUT_WORKSPACE_ROOT before downloading optional test dependencies' }
if ($env:LOCALCUT_WORKSPACE_ROOT -notmatch '^(?:[A-Za-z]:[\\/]|\\\\)') { throw 'LOCALCUT_WORKSPACE_ROOT must be absolute' }
$targetRoot = Join-Path $env:LOCALCUT_WORKSPACE_ROOT 'local-ai\cache\wheels'
for ($probe = $targetRoot; $probe; $probe = Split-Path -Parent $probe) {
    if ((Test-Path -LiteralPath $probe) -and ((Get-Item -LiteralPath $probe -Force).Attributes -band [IO.FileAttributes]::ReparsePoint)) { throw 'Download directory must not use reparse points' }
}
if ((Test-Path -LiteralPath $targetRoot) -and (Get-ChildItem -LiteralPath $targetRoot -Recurse -Force -Attributes ReparsePoint | Select-Object -First 1)) {
    throw 'Download directory must not contain reparse points'
}
New-Item -ItemType Directory -Force -Path $targetRoot | Out-Null
$packages = @(
    @('onnxruntime','1.20.1','onnxruntime-1.20.1-cp311-cp311-win_amd64.whl'),
    @('opencv-python-headless','4.11.0.86','opencv_python_headless-4.11.0.86-cp37-abi3-win_amd64.whl'),
    @('jaxlib','0.4.30','jaxlib-0.4.30-cp311-cp311-win_amd64.whl'),
    @('scipy','1.11.4','scipy-1.11.4-cp311-cp311-win_amd64.whl'),
    @('pandas','1.5.3','pandas-1.5.3-cp311-cp311-win_amd64.whl')
)
$provenance = @()
foreach ($package in $packages) {
    $metadata = Invoke-RestMethod "https://pypi.org/pypi/$($package[0])/$($package[1])/json"
    $wheel = $metadata.urls | Where-Object filename -eq $package[2]
    if (-not $wheel) { throw "Missing official wheel $($package[2])" }
    $target = Join-Path $targetRoot $wheel.filename
    if (-not (Test-Path -LiteralPath $target)) {
        $mirror = $wheel.url.Replace('https://files.pythonhosted.org','https://pypi.tuna.tsinghua.edu.cn')
        & curl.exe --noproxy '*' -L --fail --connect-timeout 15 --max-time 240 --silent --show-error $mirror -o "$target.part"
        if ($LASTEXITCODE -ne 0) { throw "Download failed $($wheel.filename)" }
        Move-Item -LiteralPath "$target.part" -Destination $target
    }
    if ((Get-FileHash -LiteralPath $target).Hash.ToLower() -ne $wheel.digests.sha256) { throw "Official SHA256 mismatch $($wheel.filename)" }
    $provenance += @{ name=$package[0]; version=$package[1]; filename=$wheel.filename; bytes=$wheel.size; sha256=$wheel.digests.sha256; official_url=$wheel.url; source='Tsinghua mirror, independently checked against official PyPI SHA256' }
    "$($wheel.filename): official PyPI SHA256 verified"
    $provenance | ConvertTo-Json -Depth 4 | Set-Content -LiteralPath (Join-Path $targetRoot 'provenance.json') -Encoding utf8
}

#Requires -Version 7.0
# SPDX-License-Identifier: GPL-3.0-only
param(
    [Parameter(Mandatory)][string]$CraftRoot,
    [string]$BuildDirectory = 'build',
    [string]$InstallDirectory = 'runtime'
)
$ErrorActionPreference = 'Stop'
$PSNativeCommandUseErrorActionPreference = $false
$repository = [IO.Path]::GetFullPath((Split-Path -Parent $PSScriptRoot))
$craft = (Resolve-Path -LiteralPath $CraftRoot).Path
function Output-Directory([string]$Path) {
    if (-not [IO.Path]::IsPathRooted($Path)) { $Path = Join-Path $repository $Path }
    $full = [IO.Path]::GetFullPath($Path)
    if (-not $full.StartsWith($repository + [IO.Path]::DirectorySeparatorChar, [StringComparison]::OrdinalIgnoreCase)) {
        throw 'Build and install directories must be inside this checkout.'
    }
    New-Item -ItemType Directory -Force -Path $full | Out-Null
    return $full
}
$build = Output-Directory $BuildDirectory
$install = Output-Directory $InstallDirectory
$cmake = Join-Path $craft 'dev-utils/bin/cmake.exe'
$ninja = Join-Path $craft 'dev-utils/bin/ninja.exe'
foreach ($tool in @($cmake, $ninja)) {
    if (-not (Test-Path -LiteralPath $tool -PathType Leaf)) { throw "Missing Craft tool: $tool" }
}
$environment = @{
    PATH = "$craft/bin;$craft/dev-utils/bin;$craft/mingw64/bin;$env:PATH"
    TEMP = (Output-Directory 'build/tmp')
    TMP = (Output-Directory 'build/tmp')
    XDG_CACHE_HOME = (Output-Directory 'build/cache')
    CCACHE_DIR = (Output-Directory 'build/cache/ccache')
    PKG_CONFIG_PATH = "$craft/lib/pkgconfig;$craft/share/pkgconfig"
}
$previous = @{}
function Run-CMake([string[]]$Arguments) {
    & $cmake @Arguments
    if ($LASTEXITCODE -ne 0) { throw "CMake failed with exit code $LASTEXITCODE" }
}
try {
    foreach ($name in $environment.Keys) {
        $previous[$name] = [Environment]::GetEnvironmentVariable($name, 'Process')
        [Environment]::SetEnvironmentVariable($name, $environment[$name], 'Process')
    }
    $configure = @('-S', $repository, '-B', $build, '-G', 'Ninja',
        "-DCMAKE_PREFIX_PATH=$craft", "-DCMAKE_MAKE_PROGRAM=$ninja",
        '-DCMAKE_BUILD_TYPE=Release', "-DCMAKE_INSTALL_PREFIX=$install",
        '-DBUILD_TESTING=OFF', '-DBUILD_RUN_QMLLINT=OFF', '-DBUILD_QCH=OFF')
    $compiler = Join-Path $craft 'mingw64/bin/g++.exe'
    if (Test-Path -LiteralPath $compiler -PathType Leaf) {
        $configure += @("-DCMAKE_CXX_COMPILER=$compiler", "-DCMAKE_C_COMPILER=$craft/mingw64/bin/gcc.exe")
    } elseif (-not (Get-Command cl.exe -ErrorAction SilentlyContinue)) {
        throw 'For an MSVC Craft installation, run this script from the matching Visual Studio developer shell.'
    }
    Run-CMake $configure
    Run-CMake @('--build', $build, '--config', 'Release', '--parallel', '2')
    Run-CMake @('--install', $build, '--config', 'Release')
    Write-Output "Built and installed this checkout to $install"
} finally {
    foreach ($name in $previous.Keys) { [Environment]::SetEnvironmentVariable($name, $previous[$name], 'Process') }
}

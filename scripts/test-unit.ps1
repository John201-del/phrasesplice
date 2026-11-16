#Requires -Version 7.0
# SPDX-License-Identifier: GPL-3.0-only
param(
    [Parameter(Mandatory)][string]$QtRoot,
    [string]$BuildDirectory = 'build/unit',
    [string]$Generator = 'Ninja'
)
$ErrorActionPreference = 'Stop'
$PSNativeCommandUseErrorActionPreference = $false
$repository = [IO.Path]::GetFullPath((Split-Path -Parent $PSScriptRoot))
$qt = (Resolve-Path -LiteralPath $QtRoot).Path
if (-not [IO.Path]::IsPathRooted($BuildDirectory)) { $BuildDirectory = Join-Path $repository $BuildDirectory }
$build = [IO.Path]::GetFullPath($BuildDirectory)
if (-not $build.StartsWith($repository + [IO.Path]::DirectorySeparatorChar, [StringComparison]::OrdinalIgnoreCase)) {
    throw 'Unit test outputs must stay inside this checkout.'
}
$environment = @{
    PATH = "$qt/bin;$qt/dev-utils/bin;$qt/mingw64/bin;$env:PATH"
    TEMP = "$build/tmp"
    TMP = "$build/tmp"
    LOCALCUT_WORKSPACE_ROOT = "$build/workspace"
    QT_QPA_PLATFORM = 'offscreen'
    QT_PLUGIN_PATH = "$qt/plugins"
    PYTHONDONTWRITEBYTECODE = '1'
    PYTHONUTF8 = '1'
}
foreach ($directory in @($build, "$build/tmp", "$build/workspace", "$build/results")) {
    New-Item -ItemType Directory -Force -Path $directory | Out-Null
}
$previous = @{}
function Run-Native([string]$Program, [string[]]$Arguments) {
    & $Program @Arguments
    if ($LASTEXITCODE -ne 0) { throw "$Program failed with exit code $LASTEXITCODE" }
}
try {
    foreach ($name in $environment.Keys) {
        $previous[$name] = [Environment]::GetEnvironmentVariable($name, 'Process')
        [Environment]::SetEnvironmentVariable($name, $environment[$name], 'Process')
    }
    $cmake = (Get-Command cmake.exe -ErrorAction Stop).Source
    $python = (Get-Command python.exe -ErrorAction Stop).Source
    $options = @("-DCMAKE_PREFIX_PATH=$qt", '-DCMAKE_BUILD_TYPE=Release', "-DPython3_EXECUTABLE=$python")
    if ($Generator -like 'Visual Studio*') {
        $options += @('-A', 'x64', '-DCMAKE_CXX_FLAGS=/utf-8')
    } elseif (Test-Path -LiteralPath "$qt/mingw64/bin/g++.exe" -PathType Leaf) {
        $options += "-DCMAKE_CXX_COMPILER=$qt/mingw64/bin/g++.exe"
    }
    $suites = @(
        @('assistantstorage', 'assistantstoragetest'),
        @('assistantpanel', 'assistantpaneltest'),
        @('assistantpanel', 'assistantconnectiondialogtest'),
        @('assistantundo', 'assistantundotest'),
        @('assistant', 'codexclienttest'),
        @('chatcompletions', 'chatcompletionsclienttest'),
        @('workbuddy', 'workbuddybridgetest'),
        @('assistantsession', 'assistantsessiontest'),
        @('videogeneration', 'ltxspacevideoprovidertest')
    )
    foreach ($name in @($suites | ForEach-Object { $_[1] + '.xml' }) + @('summary.json')) {
        $previousReport = Join-Path "$build/results" $name
        if (Test-Path -LiteralPath $previousReport -PathType Leaf) { Remove-Item -LiteralPath $previousReport }
    }
    $totals = @()
    foreach ($suite in $suites) {
        $directory, $target = $suite
        $suiteBuild = "$build/$directory"
        Run-Native $cmake (@('-S', "$repository/tests/$directory", '-B', $suiteBuild, '-G', $Generator) + $options)
        Run-Native $cmake @('--build', $suiteBuild, '--config', 'Release', '--target', $target, '--parallel', '2')
        $executable = "$suiteBuild/$target.exe"
        if ($Generator -like 'Visual Studio*') { $executable = "$suiteBuild/Release/$target.exe" }
        $report = "$build/results/$target.xml"
        Run-Native $executable @('-o', "$report,xml")
        [xml]$result = Get-Content -LiteralPath $report -Raw -Encoding UTF8
        $incidents = @($result.SelectNodes('/TestCase/TestFunction[not(@name="initTestCase") and not(@name="cleanupTestCase")]/Incident'))
        $passed = @($incidents | Where-Object { $_.type -eq 'pass' }).Count
        $skipped = @($incidents | Where-Object { $_.type -eq 'skip' }).Count
        $failed = @($incidents | Where-Object { $_.type -notin @('pass', 'skip') }).Count
        if ($passed -eq 0 -or $failed -gt 0) { throw "No passing test cases or failed assertions in $target" }
        $totals += [pscustomobject]@{ suite = $target; passed = $passed; skipped = $skipped; failed = $failed }
        Write-Output "$target : $passed passed, $skipped skipped, $failed failed (setup/cleanup excluded)"
        foreach ($skip in @($incidents | Where-Object { $_.type -eq 'skip' })) { Write-Output ("Skip reason: " + $skip.Description) }
    }
    $totals | ConvertTo-Json | Set-Content -LiteralPath "$build/results/summary.json" -Encoding utf8
    Run-Native 'powershell.exe' @('-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', "$repository/tests/distribution/check-runtime.tests.ps1",
        '-OutputDirectory', "$build/results/distribution-$([Guid]::NewGuid().ToString('N'))")
    Run-Native $python @('-c', "from pathlib import Path; import sys; paths = [p for d in ('avatar', 'local_ai', 'whisper') for p in (Path(sys.argv[1]) / d).glob('*.py')]; assert paths; [compile(p.read_bytes(), str(p), 'exec') for p in paths]; print(f'Python syntax: {len(paths)} files')", "$repository/data/scripts")
    Run-Native $python @('-B', '-c', "import sys, unittest; suite = unittest.defaultTestLoader.discover(sys.argv[1]); assert suite.countTestCases() > 0, 'No Python tests discovered'; result = unittest.TextTestRunner(verbosity=2).run(suite); sys.exit(not result.wasSuccessful() or result.testsRun == len(result.skipped))", "$repository/tests/transcriptwords")
} finally {
    foreach ($name in $previous.Keys) { [Environment]::SetEnvironmentVariable($name, $previous[$name], 'Process') }
}

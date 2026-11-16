# Building this fork

For normal use, run `Start-PhraseSplice.cmd`. The source tree alone is not a self-contained C++ SDK.

## Windows source build

Use an official [KDE Craft](https://community.kde.org/Craft) Windows x64 environment with Kdenlive's dependencies installed. Install the dependencies with `craft --install-deps kdenlive`; build **this checkout** using the script below. Building Craft's upstream `kdenlive` target alone does not build PhraseSplice.

Requires PowerShell 7 for developer scripts, Git, CMake, Ninja, Qt, KDE Frameworks, MLT, KDDockWidgets, OpenTimelineIO and the other packages required by the root CMake file. Preparing a full SDK takes substantially more disk space than running the release.

```powershell
pwsh -NoProfile -File .\scripts\build-windows.ps1 -CraftRoot E:\CraftRoot -InstallDirectory build\installed
```

Replace `E:\CraftRoot` with your real Craft installation. The script sets the build process environment, configures this repository, builds with two workers and installs its own components beneath the checkout. It does not download an SDK, mutate global PATH or supply third-party runtime DLLs.

The initial Windows application build uses this ABI family:

| Component | Version |
| --- | --- |
| Architecture / compiler | Windows x64 / MinGW GCC 14.2 |
| CMake / Ninja | 4.4.4 / 1.13.2 |
| Qt / KDE Frameworks | 6.12.0 / 6.31.0 |
| MLT / KDDockWidgets | 7.43.0 / 2.4.1 |
| Kdenlive source baseline | 26.08.2 development snapshot |

The root CMake file declares minimum versions. A different valid SDK may compile, but it is not binary-compatible by assumption: never mix a newly compiled application with older Qt DLLs or arbitrary MLT/FFmpeg packages. Validate the resulting runtime.

The Windows release installer uses the pinned artifacts in [windows.json](../distribution/windows.json). It downloads third-party runtimes from their publishers, excludes the upstream FFmpeg files, and uses the explicitly pinned GPL shared FFmpeg build. The PhraseSplice application overlay contains this fork's installed components and an application hash manifest. Every executable release must be paired with the exact source tag used to build it.

Package and test your own build:

```powershell
pwsh -NoProfile -File .\scripts\package-overlay.ps1
powershell -NoProfile -ExecutionPolicy Bypass -File .\scripts\install-windows.ps1 -OverlayArchive .\dist\phrasesplice-windows-x64-0.1.0-preview.2.zip
.\Start-PhraseSplice.cmd
```

Packaging writes a new application archive and updates the local `distribution/windows.json` hash. Do not commit that release metadata unless you intend to publish that exact build. The installer refuses to overwrite an existing runtime; use a fresh checkout or preserve and rename the old runtime before testing. Keep your user data.

## Unit tests

```powershell
pwsh -NoProfile -File .\scripts\test-unit.ps1 -QtRoot E:\CraftRoot
```

For MSVC Qt, use the matching Visual Studio installation:

```powershell
pwsh -NoProfile -File .\scripts\test-unit.ps1 -QtRoot E:\Qt\6.10.0\msvc2022_64 -Generator 'Visual Studio 17 2022'
```

The selected suites cover storage paths, assistant widgets and connection settings, grouped undo, Codex and compatible API protocols, the WorkBuddy MCP bridge, backend switching, and the public video-provider protocol. The script also checks invalid runtime manifests and Python helpers. Protocol tests use local fake servers; they do not prove a live cloud service is available. XML reports and a count summary are generated under `build/unit/results`. Python is required for the protocol fixture and transcript tests.

Optional model/media suites under `tests/localai` and `tests/avatar` need their documented local runtime and fixtures. Explicit skips are not a model acceptance pass. Native editor tests in the top-level `tests` directory require the complete editor build; some retain manual integration fixtures and are not part of the lightweight public CI.

The resource-provider credential regression compiles the real `ProviderModel` and requires a full Craft SDK, including Qt NetworkAuth and KDE Config, I18n, WidgetsAddons and KIO. It is separate from the Qt-only CI. In a fresh PowerShell window at the checkout root, use the matching MinGW Craft installation:

```powershell
$craftRoot = 'E:\CraftRoot'
$testRoot = Join-Path (Get-Location).Path 'build\resourceproviders'
New-Item -ItemType Directory -Force "$testRoot/tmp", "$testRoot/userdata" | Out-Null
$env:TEMP = "$testRoot/tmp"
$env:TMP = $env:TEMP
$env:LOCALCUT_WORKSPACE_ROOT = "$testRoot/userdata"
$env:PATH = "$craftRoot/bin;$craftRoot/mingw64/bin;$craftRoot/dev-utils/bin;$env:PATH"
& "$craftRoot/dev-utils/bin/cmake.exe" -S tests/resourceproviders -B $testRoot -G Ninja "-DCMAKE_PREFIX_PATH=$craftRoot" "-DCMAKE_CXX_COMPILER=$craftRoot/mingw64/bin/g++.exe"
if ($LASTEXITCODE -ne 0) { throw 'Provider test configuration failed.' }
& "$craftRoot/dev-utils/bin/cmake.exe" --build $testRoot --parallel 2
if ($LASTEXITCODE -ne 0) { throw 'Provider test build failed.' }
& "$testRoot/resourceprovidertest.exe" -o "$testRoot/result.xml,xml"
if ($LASTEXITCODE -ne 0) { throw 'Provider credential regression failed.' }
```

These eight cases use synthetic keys and a loopback HTTP server to verify missing-key rejection, environment-key substitution, and existing custom/keyless profiles. They do not contact Freesound, Pexels or Pixabay or validate a real account.

When running these additional C++ suites directly, set `LOCALCUT_WORKSPACE_ROOT` to an existing absolute writable test directory (for example, `<checkout>/build/native-workspace`). Set `LOCALCUT_NATIVE_FIXTURES` to an existing directory containing your own manual media fixtures; the native runner checks the six relative filenames listed in `tests/TestMain.cpp` and exits with an error when any are missing. Optional speech/media suites state their additional fixture and model requirements in their tests. These historical fixtures and models are not included in this repository and have not been rerun as part of the public CI. Tests read inputs from the fixture directory and write generated files under the workspace. Set `LOCALCUT_TEST_FFMPEG`, `LOCALCUT_TEST_FFPROBE` and, for simulated Python subprocesses, `LOCALCUT_TEST_PYTHON` to absolute executable paths, or make those tools available on `PATH`.

## Other platforms

The upstream [build guide](../dev-docs/build.md) remains available. PhraseSplice's additional Windows install scripts and optional model environments have not been validated on Linux or macOS. Do not interpret the upstream platform list as a certification of this fork's additions.

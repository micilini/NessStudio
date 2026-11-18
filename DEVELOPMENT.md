# Development Guide

Build, package, and verify the NessStudio 3.0.0 Windows application and its native recording engine. For product features, screenshots, and a local Visual Studio quick start, see the [main README](README.md).

## Requirements

To run: Windows 10 build 19041 or newer / Windows 11, x64. Framework-dependent packages require the **.NET 10 Desktop Runtime**. Hardware encoding additionally requires a compatible device and driver; a GPU brand alone does not guarantee encoder availability.

To build:

- Git and PowerShell.
- The .NET SDK pinned in [`global.json`](global.json).
- [Visual Studio Community 2026 or newer](https://learn.microsoft.com/en-us/dotnet/core/install/windows) with the .NET desktop development and Desktop development with C++ workloads, the v145 x64 toolset, CMake and Windows SDK 10.0.26100.0. The checked-in native preset selects the Visual Studio 2026 generator and v145 toolset. Visual Studio's CMake executable must be on `PATH` for direct `cmake` commands; the PowerShell native build script locates it automatically.
- Internet access for NuGet and the first FFmpeg restore. Native dependency sources and encoder headers are pinned locally.

## Build the app

Clone the repository, then restore, build, and publish from its root:

```powershell
git clone https://github.com/micilini/NessStudio.git
cd NessStudio
powershell -NoProfile -ExecutionPolicy Bypass -File cmake/Restore-MediaTools.ps1
dotnet build NessStudio.sln -c Release -p:Platform=x64
dotnet publish src/NessStudio.App/NessStudio.App.csproj -c Release -r win-x64 --self-contained false -o .out/app
```

Launch `.out/app/NessStudio.exe`. Publish with `--self-contained true` when you want to include the .NET runtime. Keep the complete publish directory together.

For a self-contained Windows x64 executable with ReadyToRun enabled, use the **Standalone** publish profile:

```powershell
dotnet publish src/NessStudio.App/NessStudio.App.csproj -c Release -p:PublishProfile=Standalone -p:DebugType=embedded
```

Distribute the **entire `.out/standalone` directory**. `NessStudio.exe` bundles the managed application and .NET assemblies, while native DLLs remain beside it, FFmpeg stays in `Native/FFmpeg`, and dependency notices retain their relative paths. The package is self-contained: users do not need to install the .NET Desktop Runtime. Both `IncludeNativeLibrariesForSelfExtract` and `IncludeAllContentForSelfExtract` are disabled, so these dependencies are loaded from the published directory. ReadyToRun is enabled and trimming is disabled for WPF compatibility. Do not distribute the executable alone.

MSBuild invokes `cmake/Build-Native.ps1` automatically through `NessStudio.Interop`, then copies the native runtime into the application output. You do not need to build another repository manually for the normal app build. The exact NessMuxer revision is restored from the checked-in source bundle when its submodule is absent; an existing checkout with a different revision fails explicitly.

## Generate the native DLLs and place them in the app

| Component | How it is built | Where it goes |
|---|---|---|
| `libnessstudio.dll` | Built from `native/libnessstudio` by `cmake/Build-Native.ps1` | Beside `NessStudio.exe`; MSBuild copies it automatically |
| NessMuxer | Built **statically into `libnessstudio.dll`** from `native/deps/NessMuxer` | No separate `NessMuxer.dll` is required by the V3 app |
| `libx264-164.dll`, C++ runtime DLLs | Staged with the native library | Beside `libnessstudio.dll`; keep the accompanying licenses and source archive |
| `e_sqlcipher.dll` | Pinned database runtime in `cmake/sqlcipher/runtime/win-x64` | Copied automatically into the app output |
| `ffmpeg.exe` | Restored by `cmake/Restore-MediaTools.ps1` | Copied with its notices into `Native/FFmpeg` in the app output |

To generate a standalone native SDK, including the DLL, public header, import library, CLI and redistribution notices:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File cmake/Build-Native.ps1 -Configuration Release -StageDirectory .out/libnessstudio-sdk -RunTests -Sdk
```

The compiler output is `.out/native/windows-x64/Release`; the ready-to-use SDK is `.out/libnessstudio-sdk`. For manual deployment into an already published application, copy the **runtime files from the SDK root** beside `NessStudio.exe`, preserving their relative location and notices. `include/` and `lib/` are development files and need not ship with the application. Use matching x64 Release builds, and do not substitute a DLL from a different ABI or dependency revision.

To build **NessMuxer.dll for a separate consumer**, run these commands in the [NessMuxer repository](https://github.com/micilini/NessMuxer):

```powershell
cmake --preset windows-x64
cmake --build --preset windows-x64 --parallel 3
ctest --preset windows-x64 --no-tests=error
cmake --install .out/windows-x64 --config Release --prefix .out/package
```

That DLL and its runtime dependencies are in `.out/package/bin`, its import library in `.out/package/lib`, and public headers in `.out/package/include`. Place the DLLs beside the executable that consumes **NessMuxer directly**. Copying `NessMuxer.dll` into NessStudio does not replace the statically linked implementation: update the pinned source and rebuild libnessstudio instead.

See the [libnessstudio README](native/libnessstudio/README.md) for C integration, ownership rules, capture policies and native tests.

## Automated builds and verification

The [Windows workflow](.github/workflows/build-windows.yml) builds the native SDK, runs CTest and the managed ABI probe against both the build output and the published runtime, builds/publishes the WPF app, verifies third-party notices, and uploads the SDK and application packages. Artifacts are available on the workflow run page. CI excludes three scene tests labeled `hardware`, which require a D3D11 video device. Local `-RunTests` includes them by default; add `-SkipHardwareTests` on machines without that device. CI does not replace tests on physical devices.

Local checks:

```powershell
dotnet run --project tests/NessStudio.AbiProbe -c Release
powershell -NoProfile -ExecutionPolicy Bypass -File cmake/Test-ThirdPartyNotices.ps1 -PublishedDirectory .out/app
```

On a Windows desktop with a camera, microphone, system audio endpoint and supported hardware encoder, `dotnet run --project tests/NessStudio.UiProbe -c Release -- C:/ness-ui-check` exercises the splash, independent previews, source failure/recovery and window restoration. It records all 15 nonempty source combinations in each of the four modes, then decodes every retained track and checks its duration and video frame count. Use a new output directory: the probe opens test windows and writes short recordings to an isolated profile. Append `Automatic` or `FullSoftware` to observe both previews for eight minutes, with frame-progress checks. These interactive checks are separate from CI.

For the About log preference, recording options menu and export folder controls, append `workflow C:/path/to/existing-recording` instead. Supply a finalized recording containing screen, webcam, microphone and system audio tracks. This check exports into its new evidence directory, decodes each result, and verifies that Explorer opens the correct destination for single-file and separate-track exports. It closes only the Explorer windows opened for those test destinations.

The app's splash and About windows obtain `3.0.0` from the assembly version metadata. Native ABI versions and project/database schema versions are separate contracts and do not track the product's major version.

## Repository layout

| Directory | Purpose |
|---|---|
| `src/NessStudio.App` | WPF screens, view models, application composition, assets and preview presentation |
| `src/NessStudio.Core` | Domain models and recording/scene contracts |
| `src/NessStudio.Infrastructure` | Native recording orchestration, project manifests/recovery, database and export services |
| `src/NessStudio.Interop` | C ABI declarations, native library loading, handle ownership and engine/session/job wrappers |
| `native/libnessstudio` | Engine implementation and public C header |
| `native/nessstudio-cli` | Command-line host for engine diagnostics, recording and jobs |
| `native/tests`, `tests` | Native and managed contract/regression tests |
| `native/deps` | Pinned dependency sources, headers, runtimes and source bundles |
| `cmake` | Reproducible build, runtime staging and notice verification |
| `licenses` | Redistributable third-party notices |
| `.github/workflows` | CI build/test/package jobs |
| `images` | Screenshots and README artwork |
| `.out` | Generated native builds, SDK staging and published application packages (ignored by Git) |
| `README.md`, `DEVELOPMENT.md` | Product overview and development/packaging instructions |

`NessStudio.App`, `NessStudio.Infrastructure`, and `NessStudio.Interop` target `net10.0-windows10.0.19041.0`; `NessStudio.Core` targets `net10.0`. The shared build configuration selects C# 14 and x64. libnessstudio exposes a C ABI and is implemented in C++20.

RGB patterns and synthetic capture windows exist only in test fixtures; they are not part of the application recording flow or the published runtime package.

## Database runtime

The app uses the checked-in, SHA-256-verified SQLCipher runtime. To rebuild it, use a Visual Studio x64 developer shell, Python 3.10+ and CMake:

```powershell
python cmake/sqlcipher/build_runtime.py --work-directory C:/ness-sqlcipher/work --cache C:/ness-sqlcipher/cache --output C:/ness-sqlcipher/package
```

The recipe source is [`cmake/sqlcipher/build_runtime.py`](cmake/sqlcipher/build_runtime.py).

Use new work/output directories. The recipe downloads the exact sources/tools from `cmake/sqlcipher/sources.lock.json`, builds and tests its dependencies, and emits provenance. Replacing the committed runtime also requires updating its SHA-256 record and validating existing encrypted databases; the normal app build does not rebuild cryptography.

For package license requirements, see [Licenses and Redistributions](README.md#licenses-and-redistributions).

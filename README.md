<p align="center"><img width="128" src="images/logo-nessstudio.png" alt="NessStudio"></p>

# NessStudio 3.0.0

NessStudio is a Windows recording application for screens, webcams, microphones and system audio. Version 3 moves capture, processing, encoding and recording lifecycle into **libnessstudio**, a native library with a public C ABI. The WPF application controls the engine through .NET interop.

## What V3 does

- Record a monitor or a selected screen region, with cursor capture.
- Record a webcam, microphone and system audio independently or together, including audio-only sessions.
- Keep separate source tracks or record a composed scene, with screen/camera layouts and previews.
- Pause and resume through a shared session clock; write manifests and recovery checkpoints for interrupted recordings.
- Encode H.264 through Intel Quick Sync, NVIDIA NVENC, AMD AMF or CPU x264, according to the selected policy and actual device capabilities.
- Provide explicit GPU, GPU capture with software encoding, and full software modes. Automatic mode can select a software route when hardware capabilities are unavailable; invalid configurations and driver failures remain visible errors.
- Manage recorded projects, generate thumbnails, and export video, audio or separate editing tracks using the pinned FFmpeg runtime.

Screens and camera sources produce Matroska tracks; microphone and loopback audio are stored as WAV. Scene recordings can also produce `program.mkv`. Project/session manifests describe the files, timestamps and selected routes. Export formats include MP4, MKV, MP3 and WAV.

The application and capture engine currently target **Windows x64**. Neither Linux nor macOS capture backends are implemented in libnessstudio. NessMuxer is a separate project with its own platform support. A successful build does not certify every camera, driver, GPU or recording duration.

## Requirements

To run: Windows 10 build 19041 or newer / Windows 11, x64. Framework-dependent packages require the **.NET 10 Desktop Runtime**. Hardware encoding additionally requires a compatible device and driver; a GPU brand alone does not guarantee encoder availability.

To build:

- Git and PowerShell.
- The .NET SDK pinned in [`global.json`](global.json).
- Visual Studio 2026 with Desktop development with C++, the v145 x64 toolset, CMake and Windows SDK 10.0.26100.0. Visual Studio's CMake executable must be on `PATH` for direct `cmake` commands; the PowerShell native build script locates it automatically.
- Internet access for NuGet and the first FFmpeg restore. Native dependency sources and encoder headers are pinned locally.

## Build the app

Run from the repository root:

```powershell
git clone https://github.com/micilini/NessStudio.git
cd NessStudio
powershell -NoProfile -ExecutionPolicy Bypass -File cmake/Restore-MediaTools.ps1
dotnet build NessStudio.sln -c Release -p:Platform=x64
dotnet publish src/NessStudio.App/NessStudio.App.csproj -c Release -r win-x64 --self-contained false -o .out/app
```

Launch `.out/app/NessStudio.exe`. Publish with `--self-contained true` when you want to include the .NET runtime. Keep the complete publish directory together.

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

The app's splash and About windows obtain `3.0.0` from the assembly version metadata. Native ABI versions and project/database schema versions are separate contracts and do not track the product's major version.

## Repository layout

| Directory | Purpose |
|---|---|
| `src` | WPF app, domain models, infrastructure and native interop |
| `native/libnessstudio` | Engine implementation and public C header |
| `native/tests`, `tests` | Native and managed contract/regression tests |
| `native/deps` | Pinned dependency sources, headers, runtimes and source bundles |
| `cmake` | Reproducible build, runtime staging and notice verification |
| `licenses` | Redistributable third-party notices |
| `.github/workflows` | CI build/test/package jobs |

RGB patterns and synthetic capture windows exist only in test fixtures; they are not part of the application recording flow or the published runtime package.

## Database runtime

The app uses the checked-in, SHA-256-verified SQLCipher runtime. To rebuild it, use a Visual Studio x64 developer shell, Python 3.10+ and CMake:

```powershell
python cmake/sqlcipher/build_runtime.py --work-directory C:/ness-sqlcipher/work --cache C:/ness-sqlcipher/cache --output C:/ness-sqlcipher/package
```

Use new work/output directories. The recipe downloads the exact sources/tools from `cmake/sqlcipher/sources.lock.json`, builds and tests its dependencies, and emits provenance. Replacing the committed runtime also requires updating its SHA-256 record and validating existing encrypted databases; the normal app build does not rebuild cryptography.

## License

NessStudio's own source is covered by [MIT](LICENSE). Bundled components retain their own licenses, including x264/FFmpeg GPL terms and the notices for SQLCipher, OpenSSL, SDK headers and managed packages. The native staging and publish steps preserve these notices and the supplied x264 source archive. The MIT license does not override the redistribution requirements of bundled dependencies.

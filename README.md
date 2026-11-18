<p align="center">
  <img width="128" align="center" src="images/logo-nessstudio.png">
</p>

<h1 align="center">
  NessStudio For Windows (2.0.0 Beta)
</h1>

<p align="center">
  Capture everything in high quality :)
</p>

<p align="center">
  <a href="https://micilini.com/apps/nessstudio" target="_blank">
    <img src="images/buttonDownload.png" width="300" alt="Download Link" />
  </a>
</p>

[![.NET 8](https://img.shields.io/badge/.NET-8.0-512BD4?style=flat-square&logo=dotnet)](https://dotnet.microsoft.com/)
[![Platform](https://img.shields.io/badge/platform-Windows%2010%2F11-0078D4?style=flat-square&logo=windows)](https://www.microsoft.com/windows)
[![WPF](https://img.shields.io/badge/UI-WPF-68217A?style=flat-square)](https://learn.microsoft.com/en-us/dotnet/desktop/wpf/)
[![WGC](https://img.shields.io/badge/capture-Windows%20Graphics%20Capture-00ADEF?style=flat-square)](https://learn.microsoft.com/en-us/uwp/api/windows.graphics.capture)
[![NessMuxer](https://img.shields.io/badge/muxer-NessMuxer-orange?style=flat-square)](https://github.com/micilini/NessMuxer)
[![FFmpeg Export](https://img.shields.io/badge/export-FFmpeg-007808?style=flat-square&logo=ffmpeg)](https://ffmpeg.org/)
[![License](https://img.shields.io/badge/license-MIT-green?style=flat-square)](https://github.com/micilini/NessStudio/blob/main/LICENSE)
[![Version](https://img.shields.io/badge/version-2.0.0--beta-blue?style=flat-square)](https://github.com/micilini/NessStudio/releases)
[![Status](https://img.shields.io/badge/status-public%20beta-yellow?style=flat-square)]()

---

> ⚠️ **Public Beta**
>
> NessStudio 2.0.0 is a major architecture update. The current generation is focused on native screen capture, independent recording tracks, reliable session metadata, recording previews, and FFmpeg-based export workflows.

---

# NessStudio

**NessStudio** is a Windows-native recording application capable of capturing your screen, webcam, microphone, and system audio.

It is ideal for tutorials, classes, onboarding, technical support, product demos, software demonstrations, gameplay clips, and quick everyday recordings.

## Highlights

- **Windows-native screen capture:** screen recording is powered by Windows Graphics Capture (WGC).
- **Native muxing path:** screen output is encoded and muxed through [NessMuxer](https://github.com/micilini/NessMuxer).
- **Independent capture channels:** screen, webcam, microphone, and system audio can be recorded alone or together.
- **Draw Area support:** capture a full monitor or a custom screen region.
- **Draw Area overlay:** custom-region recordings display a visual capture overlay while recording.
- **Pause / Resume:** pause and continue active sessions without losing the recording state.
- **Master Session Clock:** sessions store a real start time, stop time, and duration reference for better export sync.
- **Recording previews:** thumbnails are generated from the screen track first, with webcam fallback.
- **Project library:** recent recordings can be opened, exported, or deleted from the home screen.
- **FFmpeg export:** generate final MP4, MKV, MP3, WAV, or separated editing tracks from a recording session.
- **Diagnostics:** optional debug logs help validate capture behavior and troubleshoot issues.

## Application Images

<div style="display:flex;flex-wrap:wrap;gap:10px;justify-content:center;">
  <div style="flex:1 1 220px;max-width:32%;aspect-ratio:16/9;overflow:hidden;border-radius:8px;">
    <img src="./images/screen_01.png" alt="Image 1" style="width:100%;height:100%;object-fit:cover;display:block;">
  </div>
  <div style="flex:1 1 220px;max-width:32%;aspect-ratio:16/9;overflow:hidden;border-radius:8px;">
    <img src="./images/screen_02.png" alt="Image 2" style="width:100%;height:100%;object-fit:cover;display:block;">
  </div>
  <div style="flex:1 1 220px;max-width:32%;aspect-ratio:16/9;overflow:hidden;border-radius:8px;">
    <img src="./images/screen_03.png" alt="Image 3" style="width:100%;height:100%;object-fit:cover;display:block;">
  </div>
  <div style="flex:1 1 220px;max-width:32%;aspect-ratio:16/9;overflow:hidden;border-radius:8px;">
    <img src="./images/screen_04.png" alt="Image 4" style="width:100%;height:100%;object-fit:cover;display:block;">
  </div>
</div>

## Recording Sources

A recording session may contain one or more of these tracks:

| Source | Output |
|---|---|
| Screen | `screen.mkv` |
| Webcam | `webcam.mp4` |
| Microphone | `mic.wav` |
| System audio | `system.wav` |
| Metadata | `manifest.json` and `session.manifest.json` |
| Preview | `preview.png` when a video track exists |

Audio-only sessions are supported. In that case, no visual preview is generated.

## Export Options

NessStudio 2.0.0 adds a dedicated export workflow based on the recording manifest.

### Single File

Create one shareable file from selected video and audio sources.

Supported video layouts:

| Layout | Description |
|---|---|
| No video · Audio only | Exports only the selected audio tracks |
| Screen only | Exports the screen track |
| Webcam only | Exports the webcam track |
| Screen + Webcam · Picture-in-picture | Places webcam over the screen |
| Screen + Webcam · Side by side | Places screen and webcam next to each other |

Supported audio modes:

| Mode | Description |
|---|---|
| No audio | Exports video without audio |
| Microphone only | Uses only `mic.wav` |
| System audio only | Uses only `system.wav` |
| Microphone + System audio | Mixes both audio tracks |

Supported containers:

| Container | Use case |
|---|---|
| MP4 | Recommended video export |
| MKV | Video export with Matroska container |
| MP3 | Audio-only export |
| WAV | Audio-only lossless export |

Supported quality presets:

| Type | Presets |
|---|---|
| Video | Fast, Balanced, High quality |
| MP3 | 128 kbps, 192 kbps, 320 kbps |
| WAV | 44.1 kHz / 16-bit, 48 kHz / 16-bit, 48 kHz / 24-bit |

### Separate Tracks

Export synchronized editing assets into a folder:

| Output | Description |
|---|---|
| `screen.mp4` | Screen track converted/copied for editing |
| `webcam.mp4` | Webcam track copied for editing |
| `audio_mix.wav` | Mixed microphone/system audio |

## Architecture Overview

```text
Screen (WGC)      ──→ WgcScreenCapturePipe       ──→ NessMuxerWriter ──→ screen.mkv
Webcam (WinRT)    ──→ MediaCaptureWebcamSession  ─────────────────────→ webcam.mp4
Mic (WASAPI)      ──→ MicCaptureService          ─────────────────────→ mic.wav
System (WASAPI)   ──→ SystemLoopbackService      ─────────────────────→ system.wav

Session clock     ──→ StartedAtUtc / StoppedAtUtc / DurationMs
Manifest writer   ──→ manifest.json + session.manifest.json
Project ingest    ──→ preview.png + local project database
Export service    ──→ FFmpeg final output / separated tracks
```

The screen track uses [NessMuxer](https://github.com/micilini/NessMuxer) as the encoding and muxing backend. `NessMuxer.dll` is a standalone native C library used by the screen recording pipeline.

FFmpeg is used for export only. The application no longer depends on FFmpeg as the main screen-recording engine.

## What Changed Since Version 1.0.0

Version 2.0.0 is a major update over the original 1.0.0 release.

### Native Recording Pipeline

- Replaced the old FFmpeg-based recording path with a Windows Graphics Capture pipeline.
- Added `WgcScreenCapturePipe` for screen capture.
- Added `NessMuxerInterop` and `NessMuxerWriter` for native screen encoding/muxing.
- Added Media Foundation helpers for thumbnail generation and video metadata reading.
- Added a persistent screen capture session with improved pause/resume behavior.
- Added a recording warmup fix so the screen track does not start late after pressing record.

### Independent Recording Channels

- Screen, webcam, microphone, and system audio now behave as independent tracks.
- Screen output is stored as `screen.mkv`.
- Webcam output is stored as `webcam.mp4`.
- Microphone output is stored as `mic.wav`.
- System audio output is stored as `system.wav`.
- Audio-only recordings are valid sessions.

### Session Manifest 2

- Added manifest version 2.
- Added `StartedAtUtc`, `StoppedAtUtc`, and `DurationMs`.
- Added per-track metadata for screen, webcam, microphone, and system audio.
- Added crop metadata for Draw Area recordings.
- Preserved compatibility by writing both `manifest.json` and `session.manifest.json`.

### Master Session Clock

- Added a real session-duration reference independent of individual track lengths.
- The app records the true start/stop time of the recording session.
- Export uses the session duration when available to reduce timing drift between video and audio.

### Draw Area Improvements

- Added a visual overlay for custom region recordings.
- Overlay appears during recording, hides on pause, returns on resume, and closes on stop.
- Added protection for invalid Draw Area selections that span multiple monitors or fall outside the selected screen.

### Export Workflow

- Added a dedicated Export Recording window.
- Added Single file export.
- Added Separate tracks export.
- Added MP4 and MKV video export.
- Added MP3 and WAV audio-only export.
- Added screen-only, webcam-only, picture-in-picture, and side-by-side layouts.
- Added microphone/system audio mix controls.
- Added audio quality presets for MP3 and WAV.
- Added validation to prevent empty exports, such as “no video + no audio”.
- Added FFmpeg discovery under `Native/FFmpeg/ffmpeg.exe`.

### Project Library Improvements

- Replaced the old delete-only action with a more complete options menu.
- Added Open, Export, and Delete actions for recent recordings.
- Improved thumbnail loading and fallback behavior.
- Improved project deletion flow.

### Preview Improvements

- Recording previews now prefer `screen.mkv` first.
- If no screen track exists, previews fall back to `webcam.mp4`.
- Audio-only recordings intentionally show no visual preview.

### Preferences and Diagnostics

- Added recording preferences persisted as JSON.
- Added selectable FPS options.
- Added selectable countdown timer options.
- Added debug log toggling from the About window.
- Added performance/diagnostic helpers for recording validation.

### User Interface Refresh

- Updated the Home screen visual style.
- Updated the Menu/sidebar visual style.
- Updated Recent Recordings cards.
- Updated About screen.
- Updated Recording screen.
- Updated Saving Recording screen.
- Updated Splash screen.
- Added a modern dark theme with rounded panels, improved spacing, and consistent red accent styling.

### Project Structure

- Removed old FFmpeg recording helper classes from the previous pipeline.
- Added the `Recording/` namespace with session, WGC, Media Foundation, and NessMuxer-related components.
- Added `View/ExportScreen`.
- Added `View/DrawAreaScreen/CaptureOverlayWindow`.
- Added `View/SavingRecordingScreen`.
- Added new image assets for detected track cards and export UI.
- Updated project settings to target `net8.0-windows10.0.19041.0`.

## How to Run Locally

**Requirements:**

- Windows 10 or 11 x64
- Visual Studio Community 2022 or newer
- .NET 8 SDK / runtime support
- .NET desktop development workload

**Steps:**

1. Clone the repository.
2. Open `NessStudio.sln` in Visual Studio.
3. Make sure `NessMuxer.dll` exists under `NessStudio/Native/NessMuxer/`.
4. For export support, place `ffmpeg.exe` under `NessStudio/Native/FFmpeg/ffmpeg.exe`.
5. Build and run the project with `F5`.

The native `NessMuxer.dll` dependency is copied to the output directory during build. FFmpeg is used by the export workflow and is copied when present.

## Generated Recording Folder Example

```text
Recording Folder/
├─ screen.mkv
├─ webcam.mp4
├─ mic.wav
├─ system.wav
├─ manifest.json
├─ session.manifest.json
├─ preview.png
└─ Exports/
   ├─ recording_export.mp4
   └─ recording_tracks/
      ├─ screen.mp4
      ├─ webcam.mp4
      └─ audio_mix.wav
```

## Current Beta Notes

NessStudio 2.0.0 is ready for public beta testing, but it should still be labeled as beta.

Areas still worth testing before calling it stable:

- Long recordings with screen + webcam + microphone + system audio.
- Screen frame pacing and timeline seek behavior in `screen.mkv`.
- Export sync on longer gameplay or tutorial sessions.
- Clean installation on a fresh Windows 10/11 machine.
- Missing-dependency behavior when `ffmpeg.exe` or `NessMuxer.dll` is not present.
- Different monitor setups, DPI scales, and Draw Area selections.

## Built With

- C# / .NET 8
- Windows Presentation Foundation (WPF)
- SQLite
- Windows Graphics Capture (WGC)
- NessMuxer — native C screen encoding/muxing backend
- FFmpeg — final export workflow
- NAudio — WASAPI microphone and system audio capture
- MaterialDesignThemes — WPF UI components

## Version History

### Version 2.0.0 Beta

Major recording architecture, export, sync, and UI update. Adds independent tracks, WGC/NessMuxer screen recording, manifest v2, master session clock, Draw Area overlay, FFmpeg export window, audio-only export, separate tracks export, and refreshed application screens.

### Version 1.1.0

Introduced the native recording direction using Windows Graphics Capture and NessMuxer, replacing the old FFmpeg-first recording approach and improving session metadata/control.

### Version 1.0.0

Initial public version. Provided Windows screen, webcam, microphone, and system audio recording with simple project/session output and FFmpeg-based workflow.

## Contributing

Want to create new features for **NessStudio**? Create a new feature branch and submit a Pull Request. Feel free to open issues for bug fixes, improvements, or feature requests.

## License

This project is open-source and available under the [MIT License](https://github.com/micilini/NessStudio/blob/main/LICENSE).

---

## Version 3.0.0 — Native Recording Engine

[![.NET 10](https://img.shields.io/badge/.NET-10.0-512BD4?style=flat-square&logo=dotnet)](global.json)
[![Version 3.0.0](https://img.shields.io/badge/version-3.0.0-blue?style=flat-square)](https://github.com/micilini/NessStudio/releases)

The presentation above preserves the Version 2.0.0 Beta documentation and screenshots. **The current `main` branch is Version 3.0.0.** The requirements, architecture, build commands and DLL locations in this section replace the V2 instructions above; use this section when building or deploying the current app.

NessStudio is a Windows recording application for screens, webcams, microphones and system audio. Version 3 moves capture, processing, encoding and recording lifecycle into **libnessstudio**, a native library with a public C ABI. The WPF application controls the engine through .NET interop.

### What V3 does

- Record a monitor or a selected screen region, with cursor capture.
- Record a webcam, microphone and system audio independently or together, including audio-only sessions.
- Keep separate source tracks or record a composed scene, with screen/camera layouts and previews.
- Pause and resume through a shared session clock; write manifests and recovery checkpoints for interrupted recordings.
- Encode H.264 through Intel Quick Sync, NVIDIA NVENC, AMD AMF or CPU x264, according to the selected policy and actual device capabilities.
- Provide explicit GPU, GPU capture with software encoding, and full software modes. Automatic mode can select a software route when hardware capabilities are unavailable; invalid configurations and driver failures remain visible errors.
- Manage recorded projects, generate thumbnails, and export video, audio or separate editing tracks using the pinned FFmpeg runtime.

Screens and camera sources produce Matroska tracks; microphone and loopback audio are stored as WAV. Scene recordings can also produce `program.mkv`. Project/session manifests describe the files, timestamps and selected routes. Export formats include MP4, MKV, MP3 and WAV.

The Windows recorder keeps the compact four-card layout, with independent screen and webcam previews. New recording workspaces are stored in `%LOCALAPPDATA%\NessStudio\Recordings`. Detailed diagnostics are written to `%LOCALAPPDATA%\NessStudio\Logs\recording-debug.log` only while **Enable Debug Logs** is checked in About. Disabling it stops new diagnostic entries without deleting existing logs. Failures are recorded separately in `recording-errors.log`. Existing projects retain their original locations, and recovery also checks the former `Documents\NessStudio\Recordings` folder. Export offers **Open Folder** beside **Browse** and automatically opens the destination after successful validation of the exported files.

The application and capture engine currently target **Windows x64**. Neither Linux nor macOS capture backends are implemented in libnessstudio. NessMuxer is a separate project with its own platform support. A successful build does not certify every camera, driver, GPU or recording duration.

### Requirements

To run: Windows 10 build 19041 or newer / Windows 11, x64. Framework-dependent packages require the **.NET 10 Desktop Runtime**. Hardware encoding additionally requires a compatible device and driver; a GPU brand alone does not guarantee encoder availability.

To build:

- Git and PowerShell.
- The .NET SDK pinned in [`global.json`](global.json).
- Visual Studio 2026 with Desktop development with C++, the v145 x64 toolset, CMake and Windows SDK 10.0.26100.0. Visual Studio's CMake executable must be on `PATH` for direct `cmake` commands; the PowerShell native build script locates it automatically.
- Internet access for NuGet and the first FFmpeg restore. Native dependency sources and encoder headers are pinned locally.

### Build the app

Run from the repository root:

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

### Generate the native DLLs and place them in the app

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

### Automated builds and verification

The [Windows workflow](.github/workflows/build-windows.yml) builds the native SDK, runs CTest and the managed ABI probe against both the build output and the published runtime, builds/publishes the WPF app, verifies third-party notices, and uploads the SDK and application packages. Artifacts are available on the workflow run page. CI excludes three scene tests labeled `hardware`, which require a D3D11 video device. Local `-RunTests` includes them by default; add `-SkipHardwareTests` on machines without that device. CI does not replace tests on physical devices.

Local checks:

```powershell
dotnet run --project tests/NessStudio.AbiProbe -c Release
powershell -NoProfile -ExecutionPolicy Bypass -File cmake/Test-ThirdPartyNotices.ps1 -PublishedDirectory .out/app
```

On a Windows desktop with a camera, microphone, system audio endpoint and supported hardware encoder, `dotnet run --project tests/NessStudio.UiProbe -c Release -- C:/ness-ui-check` exercises the splash, independent previews, source failure/recovery and window restoration. It records all 15 nonempty source combinations in each of the four modes, then decodes every retained track and checks its duration and video frame count. Use a new output directory: the probe opens test windows and writes short recordings to an isolated profile. Append `Automatic` or `FullSoftware` to observe both previews for eight minutes, with frame-progress checks. These interactive checks are separate from CI.

For the About log preference, recording options menu and export folder controls, append `workflow C:/path/to/existing-recording` instead. Supply a finalized recording containing screen, webcam, microphone and system audio tracks. This check exports into its new evidence directory, decodes each result, and verifies that Explorer opens the correct destination for single-file and separate-track exports. It closes only the Explorer windows opened for those test destinations.

The app's splash and About windows obtain `3.0.0` from the assembly version metadata. Native ABI versions and project/database schema versions are separate contracts and do not track the product's major version.

### Repository layout

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

### Database runtime

The app uses the checked-in, SHA-256-verified SQLCipher runtime. To rebuild it, use a Visual Studio x64 developer shell, Python 3.10+ and CMake:

```powershell
python cmake/sqlcipher/build_runtime.py --work-directory C:/ness-sqlcipher/work --cache C:/ness-sqlcipher/cache --output C:/ness-sqlcipher/package
```

Use new work/output directories. The recipe downloads the exact sources/tools from `cmake/sqlcipher/sources.lock.json`, builds and tests its dependencies, and emits provenance. Replacing the committed runtime also requires updating its SHA-256 record and validating existing encrypted databases; the normal app build does not rebuild cryptography.

### V3 licenses and redistribution

NessStudio's own source is covered by [MIT](LICENSE). Bundled components retain their own licenses, including x264/FFmpeg GPL terms and the notices for SQLCipher, OpenSSL, SDK headers and managed packages. The native staging and publish steps preserve these notices and the supplied x264 source archive. The MIT license does not override the redistribution requirements of bundled dependencies.

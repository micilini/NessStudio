<p align="center">
  <img width="128" align="center" src="images/logo-nessstudio.png">
</p>

<h1 align="center">
  NessStudio For Windows (1.1.0)
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
[![License](https://img.shields.io/badge/license-MIT-green?style=flat-square)](https://github.com/micilini/NessStudio/blob/main/LICENSE)
[![Version](https://img.shields.io/badge/version-1.1.0-blue?style=flat-square)](https://github.com/micilini/NessStudio/releases)
[![Status](https://img.shields.io/badge/status-active%20development-yellow?style=flat-square)]()

---

> ⚠️ **Work in Progress**
>
> NessStudio is under active development. The current generation is focused on the native recording pipeline, independent capture channels, session metadata, recording previews, and export tools.

---

# NessStudio

**NessStudio** is a Windows-native recording application capable of capturing your screen, webcam, microphone, and system audio.

It is ideal for tutorials, classes, onboarding, technical support, product demos, and quick everyday recordings.

## Features

- **Capture modes:** full monitor or custom region (_Draw Area_)
- **Independent recording channels:** screen, webcam, microphone, and system audio can be recorded alone or in combination
- **Webcam + Screen:** simultaneous recording with configurable quality
- **Full audio support:** microphone and system audio are stored as dedicated `.wav` tracks
- **Pause / Resume:** pause and continue recording without losing the session
- **Windows-native screen pipeline:** built on [Windows Graphics Capture (WGC)](https://learn.microsoft.com/en-us/uwp/api/windows.graphics.capture)
- **NessMuxer integration:** screen video is encoded and muxed by [NessMuxer](https://github.com/micilini/NessMuxer), a standalone C library
- **Session manifest:** each recording generates `manifest.json` and `session.manifest.json` with metadata, track info, and pause intervals
- **Recording preview:** session thumbnails are generated from the screen track first, with webcam as fallback
- **Diagnostics support:** debug logs help validate capture behavior and troubleshoot issues

## Current Recording Output

A recording session may generate one or more of the following files, depending on the selected sources:

| Source | Output |
|---|---|
| Screen | `screen.mkv` |
| Webcam | `webcam.mp4` |
| Microphone | `mic.wav` |
| System audio | `system.wav` |
| Metadata | `manifest.json` and `session.manifest.json` |
| Preview | `preview.png` when a video track exists |

Audio-only sessions are valid. In that case, no visual preview is generated.

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
</div>

## Architecture Overview

```
Screen (WGC)      ──→ WgcScreenCapturePipe       ──→ NessMuxerWriter ──→ screen.mkv
Webcam (WinRT)    ──→ MediaCaptureWebcamSession  ─────────────────────→ webcam.mp4
Mic (WASAPI)      ──→ MicCaptureService          ─────────────────────→ mic.wav
System (WASAPI)   ──→ SystemLoopbackService      ─────────────────────→ system.wav

Project ingest    ──→ manifest.json              ──→ preview.png
```

The screen track uses [NessMuxer](https://github.com/micilini/NessMuxer) as the encoding and muxing backend. `NessMuxer.dll` is a standalone native C library for the screen pipeline.

## How to Run Locally

**Requirements:**

- Windows 10 or 11 (x64)
- [Visual Studio Community 2022](https://visualstudio.microsoft.com/) with the **.NET desktop development** workload
- .NET 8 SDK / runtime support

**Steps:**

1. Clone the repository.
2. Open `NessStudio.sln` in Visual Studio.
3. Build and run the project with `F5`.

The `NessMuxer.dll` native dependency is bundled under `NessStudio/Native/NessMuxer/` and is copied to the output directory during build.

## Active Roadmap

The old P3/P4 status list has been consolidated. The current roadmap is focused on P5 and the remaining UX/export work.

### Completed / Stabilized

| Phase | Status | Notes |
|---|---|---|
| P3.2 | ✅ Done | Native screen recording pipeline stabilized |
| P3.3 | ✅ Done | Screen output consolidated as `screen.mkv` |
| P3.4 | ✅ Done | Audio tracks consolidated as continuous `.wav` files |
| P3.5 | ✅ Done | Recording lifecycle and memory behavior improved |
| P3.6 | ✅ Done | Manifest updated for current track structure |
| P5.1 | ✅ Done | Screen, webcam, microphone, and system audio can work as independent channels |
| P5.2 | ✅ Done | Recording preview now prefers `Screen.File`, then falls back to `Webcam.File` through `manifest.json` |

### Still Missing

| Phase | Status | Goal |
|---|---|---|
| P5.3 | 📋 Next | Add a visual overlay for the selected Draw Area during recording |
| P5.4 | 📋 Planned | Add export flow for final `.mp4` / `.mkv` files using FFmpeg |
| P4.x | 📋 Optional polish | Keep-alive and warmup improvements for WGC, webcam, and resume behavior |

## Known Limitations

| Limitation | Status |
|---|---|
| Audio-only recordings do not generate a visual preview | Expected behavior |
| Draw Area does not yet show a dedicated capture-region overlay while recording | Planned in P5.3 |
| Final export/merge workflow is not available yet | Planned in P5.4 |
| Some pause/resume visual polish may still be improved | Optional P4 work |

## Built With

- C# / .NET 8
- Windows Presentation Foundation (WPF)
- SQLite
- [Windows Graphics Capture (WGC)](https://learn.microsoft.com/en-us/uwp/api/windows.graphics.capture)
- [NessMuxer](https://github.com/micilini/NessMuxer) — standalone C library for NV12 → H.264 → MKV encoding
- [NAudio](https://github.com/naudio/NAudio) — WASAPI audio capture
- [MaterialDesignThemes](https://github.com/MaterialDesignInXAML/MaterialDesignInXamlToolkit)

## Updates

**Version 1.1.0** — Replaced the old FFmpeg-based recording path with a Windows-native capture architecture using **Windows Graphics Capture (WGC)** and **[NessMuxer](https://github.com/micilini/NessMuxer)**. This reduces dependency on external recording subprocesses and improves control over session metadata, pause/resume, and per-source recording behavior.

## Contributing

Want to create new features for **NessStudio**? Create a new feature branch and submit a **Pull Request**. Feel free to open issues for bug fixes, improvements, or feature requests.

## License

This project is open-source and available under the [MIT License](https://github.com/micilini/NessStudio/blob/main/LICENSE).

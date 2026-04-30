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
> NessStudio is under active development. Some features are still being stabilized. See the [Known Issues](#known-issues) section before using.

---

# NessStudio

**NessStudio** is a Windows-native screen recorder capable of capturing your screen, webcam, and audio (microphone + system) simultaneously. It outputs a single `.mkv` file for video (encoded as H.264) and `.wav` files for each audio track.

Ideal for tutorials, classes, onboarding, technical support, and everyday recordings.

## Features

- **Capture modes:** full monitor or custom region (_Draw Area_)
- **Webcam + Screen:** simultaneous recording with configurable quality
- **Full Audio Support:** microphone and system audio in dedicated tracks
- **Pause / Resume:** pause and continue recording without losing the session
- **Windows-native pipeline:** built on [Windows Graphics Capture (WGC)](https://learn.microsoft.com/en-us/uwp/api/windows.graphics.capture) — no FFmpeg dependency
- **NessMuxer integration:** video is encoded and muxed by [NessMuxer](https://github.com/micilini/NessMuxer), a standalone C library (NV12 → H.264 → MKV)
- **Session manifest:** each recording generates a `session.manifest.json` with metadata, track info, and pause intervals
- **Diagnostics support:** optional debug logs for troubleshooting and validation

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
Screen (WGC)  ──→  WgcScreenCapturePipe  ──→  NessMuxerWriter  ──→  screen.mkv
Webcam (WinRT) ─→  MediaCaptureWebcamSession ──────────────────────→  webcam_seg_N.mp4 *
Mic (WASAPI)  ──→  MicCaptureService  ─────────────────────────────→  mic_seg_N.wav *
System (WASAPI)─→  SystemLoopbackService ──────────────────────────→  system_seg_N.wav *

* Multiple files per session — being resolved in P3.3/P3.4
```

The screen track uses [NessMuxer](https://github.com/micilini/NessMuxer) as the encoding/muxing backend. The `NessMuxer.dll` is a standalone native C library — no FFmpeg, no external runtime required.

## How to Run Locally

**Requirements:**
- Windows 10 or 11 (x64)
- [Visual Studio Community 2022](https://visualstudio.microsoft.com/) with the **.NET desktop development** workload

**Steps:**
1. Clone the repository
2. Open `NessStudio.sln` in Visual Studio
3. Build and run (`F5`)

The `NessMuxer.dll` is already bundled under `NessStudio/Native/NessMuxer/` — no separate build step required.

## Known Issues

The following features are **not yet fully working** in the current version. They are tracked in the active development roadmap (P3/P4):

| Issue | Status | Tracked in |
|---|---|---|
| Webcam generates N separate files per session (one per segment) | 🔧 In progress | P3.3 |
| Mic and system audio generate N separate files per session | 🔧 In progress | P3.4 |
| `Direct3D11CaptureFramePool` is recreated on every resume (~75 MB extra per resume) | 🔧 In progress | P3.5 |
| `session.manifest.json` still uses legacy `Segments[]` structure for webcam/audio | 🔧 In progress | P3.6 |
| WGC indicator (yellow border) flickers on pause/resume | 📋 Planned | P4.0 |
| Webcam LED may turn off on pause | 📋 Planned | P4.1 |
| Resume delay (~700 ms) due to warmup | 📋 Planned | P4.2 |

> The screen track (`.mkv`) is stable and produces a single continuous file per session, including across multiple pause/resume cycles.

## Built With

- C# / .NET 8
- Windows Presentation Foundation (WPF)
- SQLite
- [Windows Graphics Capture (WGC)](https://learn.microsoft.com/en-us/uwp/api/windows.graphics.capture)
- [NessMuxer](https://github.com/micilini/NessMuxer) — standalone C library for NV12 → H.264 → MKV encoding
- [NAudio](https://github.com/naudio/NAudio) — WASAPI audio capture
- [MaterialDesignThemes](https://github.com/MaterialDesignInXAML/MaterialDesignInXamlToolkit)

## Updates

**Version 1.1.0** — Replaced the FFmpeg-based pipeline with a fully Windows-native capture architecture using **Windows Graphics Capture (WGC)** and **[NessMuxer](https://github.com/micilini/NessMuxer)**. This eliminates the FFmpeg subprocess dependency, resolves intermittent recording hangs, and dramatically reduces memory usage during pause/resume cycles.

## Contributing

Want to create new features for **NessStudio**? Create a new feature branch and submit a **Pull Request**. Feel free to open issues for bug fixes or feature requests.

## License

This project is open-source and available under the [MIT License](https://github.com/micilini/NessStudio/blob/main/LICENSE).
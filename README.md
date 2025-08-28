<p align="center">
  <img width="128" align="center" src="images/logo-nessstudio.png">
</p>
<h1 align="center">
  NessStudio For Windows (1.0.0)
</h1>
<p align="center">
  Capture everything in high quality :)
</p>
<p align="center">
  <a href="https://micilini.com/apps/nessstudio" target="_blank">
    <img src="images/buttonDownload.png" width="300" alt="Download Link" />
  </a>
</p>

# NessStudio

**NessStudio** is an application made for Windows (10/11) capable of recording your screen, webcam, and audio (microphone + system) in MP4/WAV. This type of application is ideal for tutorials, classes, onboarding, technical support and quick everyday recordings.

See the benefits of using **NessStudio**:

- **Capture modes:** entire monitor or _Draw Area_ (custom region).
- **Webcam + Screen:** perform simultaneous recordings with configurable quality.
- **Full Audio Support:** record your microphone and system audio outputs to a .WAV track.
- **Pause or Resume:** pause your recordings and resume them whenever you want.
- **Ready files:** MP4 with _faststart_ (streaming-friendly) and folders per session.
- **Light and straightforward:** simple interface, countdown timer, no distractions.

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

## How to runs this application locally?

This application was made using the following technologies:

- C#
- Windows Presentation Foundation (WPF)
- SQLite

In conjunction with the following libraries:

- [FFMpeg](https://ffmpeg.org/)
- [MaterialDesignThemes](https://github.com/MaterialDesignInXAML/MaterialDesignInXamlToolkit)

To run this application on your local machine, make sure you have the latest version of **Visual Studio Community 2022**.

First of all, **clone the repository to your local machine**, and then simply open the **NessStudio.sln** file to open the project.

Don't forget to download the **FFMpeg executables** (_ffmpeg.exe, ffplay.exe, ffprobe.exe_) and place them inside the _Modules_ folder.

## Contribuite

Want to create new features for **NessStudio**? Then make sure you create a new feature (or a new translation file) and submit a new **Pull Request** (PR).

Feel free to open new **Pull Requests** (PR) whenever you create new bug fixes or future translations.

---

## License

This script is open-source and available under the MIT License.

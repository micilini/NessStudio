using DirectShowLib;
using NAudio.CoreAudioApi;
using NAudio.Wave;
using NessStudio.Models;
using NessStudio.Recording;
using NessStudio.Recording.Engines;
using NessStudio.Recording.Windows;
using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.IO;
using System.Linq;
using System.Threading.Tasks;
using System.Windows;
using System.Windows.Forms;
using static NessStudio.Recording.SessionManifest;

namespace NessStudio.ViewModel.Helpers
{
    public class RecordAssist : IDisposable, IRecorderEngine
    {
        private readonly NessStudio.Models.RecordingOutputPaths _paths;
        private readonly RecordingTargets _targets;
        private NessStudio.Models.ScreenRegion _region;
        private MediaCaptureWebcamSegmentRecorder _wgcWebcam;
        private WgcScreenCapturePipe _wgcScreen;
        private WasapiCapture _micCapture;
        private WaveFileWriter _micWriter;
        private WasapiLoopbackCapture _loopCapture;
        private WaveFileWriter _loopWriter;
        private readonly NessStudio.Models.RecordingSegmentState _seg = new NessStudio.Models.RecordingSegmentState();
        private System.Timers.Timer _loopTick;
        private NessStudio.Models.AudioClockState _clock = new NessStudio.Models.AudioClockState();
        private readonly RecordingRuntimeOptions _runtimeOptions;

        private sealed class ScreenTrackSnapshot
        {
            public int Width { get; set; }
            public int Height { get; set; }
            public int Fps { get; set; }
            public long FrameCount { get; set; }
            public int? StrideY { get; set; }
            public int? StrideUV { get; set; }
            public string PixelFormat { get; set; }
            public bool IsRawIntermediate { get; set; }
        }

        public RecordAssist(
            RecordingOutputPaths paths,
            RecordingTargets targets,
            System.Windows.Rect? cropPx = null,
            RecordingRuntimeOptions runtimeOptions = null)
        {
            _paths = paths ?? throw new ArgumentNullException(nameof(paths));
            _targets = targets ?? throw new ArgumentNullException(nameof(targets));
            _runtimeOptions = runtimeOptions ?? new RecordingRuntimeOptions();
            _region = new ScreenRegion(targets.Screen, cropPx);
        }

        public async Task PrepareAsync()
        {
            if (string.IsNullOrWhiteSpace(_targets.WebcamName))
                return;

            _wgcWebcam ??= new MediaCaptureWebcamSegmentRecorder();

            string outFile = _paths.WebcamContinuous();

            DebugLog.Write(
                $"[RecordAssist] PrepareAsync begin | webcam={_targets.WebcamName} | outFile={outFile}");

            await _wgcWebcam.PrepareAsync(_targets.WebcamName, outFile);

            DebugLog.Write("[RecordAssist] PrepareAsync end");
        }

        public async Task StartAsync()
        {
            if (_seg.IsRunning)
                return;

            DebugLog.Write(
                $"[RecordAssist] StartAsync begin | " +
                $"segment={_seg.SegmentIndex} | " +
                $"screen={_targets.Screen != null} | " +
                $"webcam={!string.IsNullOrWhiteSpace(_targets.WebcamName)} | " +
                $"mic={!string.IsNullOrWhiteSpace(_targets.MicDeviceId)} | " +
                $"system={!string.IsNullOrWhiteSpace(_targets.LoopbackDeviceId)}");

            _seg.Start();

            try
            {
                DebugLog.Write("[RecordAssist] StartScreenSegment()");
                StartScreenSegment();
                DebugLog.Write("[RecordAssist] StartScreenSegment() OK");
            }
            catch (Exception ex)
            {
                DebugLog.Write("[RecordAssist] StartScreenSegment() ERROR:\n" + ex);
                _seg.TryStop();
                throw new InvalidOperationException("Screen capture failed to start.", ex);
            }

            try
            {
                DebugLog.Write("[RecordAssist] StartWebcamSegment()");
                await StartWebcamSegmentAsync();
                DebugLog.Write("[RecordAssist] StartWebcamSegment() OK");
            }
            catch (Exception ex)
            {
                DebugLog.Write("[RecordAssist] StartWebcamSegment() ERROR:\n" + ex);
                try { StopScreenSegment(); } catch { }
                _seg.TryStop();
                throw new InvalidOperationException("Webcam capture failed to start.", ex);
            }

            try
            {
                DebugLog.Write("[RecordAssist] StartMicSegment()");
                StartMicSegment();
                DebugLog.Write("[RecordAssist] StartMicSegment() OK");
            }
            catch (Exception ex)
            {
                DebugLog.Write("[RecordAssist] StartMicSegment() ERROR:\n" + ex);
                try { await StopWebcamSegmentAsync(); } catch { }
                try { StopScreenSegment(); } catch { }
                _seg.TryStop();
                throw new InvalidOperationException("Microphone capture failed to start.", ex);
            }

            try
            {
                DebugLog.Write("[RecordAssist] StartLoopbackSegment()");
                StartLoopbackSegment();
                DebugLog.Write("[RecordAssist] StartLoopbackSegment() OK");
            }
            catch (Exception ex)
            {
                DebugLog.Write("[RecordAssist] StartLoopbackSegment() ERROR:\n" + ex);
                try { StopMicSegment(); } catch { }
                try { await StopWebcamSegmentAsync(); } catch { }
                try { StopScreenSegment(); } catch { }
                _seg.TryStop();
                throw new InvalidOperationException("System audio capture failed to start.", ex);
            }
        }

        public async Task PauseAsync()
        {
            if (!_seg.IsRunning || _seg.IsPaused)
                return;

            DebugLog.Write("[RecordAssist] PauseAsync begin");
            _seg.TryPause();

            try
            {
                DebugLog.Write("[RecordAssist] PauseAsync -> StopScreenSegment begin");
                await Task.Run(() => StopScreenSegment());
                DebugLog.Write("[RecordAssist] PauseAsync -> StopScreenSegment end");
            }
            catch (Exception ex)
            {
                DebugLog.Write("[RecordAssist] PauseAsync -> StopScreenSegment ERROR:\n" + ex);
                throw;
            }

            try
            {
                DebugLog.Write("[RecordAssist] PauseAsync -> StopWebcamSegmentAsync begin");
                await StopWebcamSegmentAsync();
                DebugLog.Write("[RecordAssist] PauseAsync -> StopWebcamSegmentAsync end");
            }
            catch (Exception ex)
            {
                DebugLog.Write("[RecordAssist] PauseAsync -> StopWebcamSegmentAsync ERROR:\n" + ex);
                throw;
            }

            try
            {
                DebugLog.Write("[RecordAssist] PauseAsync -> StopMicSegment begin");
                StopMicSegment();
                DebugLog.Write("[RecordAssist] PauseAsync -> StopMicSegment end");
            }
            catch (Exception ex)
            {
                DebugLog.Write("[RecordAssist] PauseAsync -> StopMicSegment ERROR:\n" + ex);
                throw;
            }

            try
            {
                DebugLog.Write("[RecordAssist] PauseAsync -> StopLoopbackSegment begin");
                StopLoopbackSegment();
                DebugLog.Write("[RecordAssist] PauseAsync -> StopLoopbackSegment end");
            }
            catch (Exception ex)
            {
                DebugLog.Write("[RecordAssist] PauseAsync -> StopLoopbackSegment ERROR:\n" + ex);
                throw;
            }

            DebugLog.Write("[RecordAssist] PauseAsync end");
            DebugLog.Flush();
        }

        public async Task ResumeAsync()
        {
            if (!_seg.IsRunning || !_seg.IsPaused)
                return;

            DebugLog.Write("[RecordAssist] ResumeAsync begin");
            RecordingPerfProbe.Mark("resume-begin", $"segment={_seg.SegmentIndex}");
            _seg.TryResume();

            RecordingPerfProbe.Mark("resume-screen-begin", $"segment={_seg.SegmentIndex}");
            StartScreenSegment();
            RecordingPerfProbe.Mark("resume-screen-end", $"segment={_seg.SegmentIndex}");

            RecordingPerfProbe.Mark("resume-webcam-begin", $"segment={_seg.SegmentIndex}");
            await StartWebcamSegmentAsync();
            RecordingPerfProbe.Mark("resume-webcam-end", $"segment={_seg.SegmentIndex}");

            RecordingPerfProbe.Mark("resume-mic-begin", $"segment={_seg.SegmentIndex}");
            StartMicSegment();
            RecordingPerfProbe.Mark("resume-mic-end", $"segment={_seg.SegmentIndex}");

            RecordingPerfProbe.Mark("resume-loopback-begin", $"segment={_seg.SegmentIndex}");
            StartLoopbackSegment();
            RecordingPerfProbe.Mark("resume-loopback-end", $"segment={_seg.SegmentIndex}");

            DebugLog.Write("[RecordAssist] ResumeAsync end");
            RecordingPerfProbe.Mark("resume-end", $"segment={_seg.SegmentIndex}");
        }

        public async Task StopAsync()
        {
            if (!_seg.IsRunning)
                return;

            _seg.TryStop();

            StopScreenSegment();

            try { _wgcScreen?.ReleaseSession(); } catch { }
            _wgcScreen = null;

            await StopWebcamSegmentAsync();
            StopMicSegment();
            StopLoopbackSegment();
            _seg.TryStop();
        }

        private void StartScreenSegment()
        {
            int recordingFps = RecordingPreferencesService.NormalizeFps(_runtimeOptions?.RecordingFps ?? 30);
            int screenWarmupMs = Math.Max(0, _runtimeOptions?.ScreenWarmupMilliseconds ?? 1200);

            if (_wgcScreen == null)
            {
                DebugLog.Write("[RecordAssist] Creating WgcScreenCapturePipe (session)");
                DebugLog.Write($"[RecordAssist] screen fps => {recordingFps}");
                DebugLog.Write($"[RecordAssist] screen warmup => {screenWarmupMs}ms");

                _wgcScreen = new WgcScreenCapturePipe(
                    _region,
                    _paths,
                    recordingFps,
                    true,
                    screenWarmupMs);

                DebugLog.Write("[RecordAssist] Calling _wgcScreen.InitializeSession() on UI thread");
                _wgcScreen.InitializeSession();
                DebugLog.Write("[RecordAssist] _wgcScreen.InitializeSession() returned");
            }
            else
            {
                DebugLog.Write("[RecordAssist] WgcScreenCapturePipe session reused");
            }

            DebugLog.Write($"[RecordAssist] Calling _wgcScreen.StartSegment({_seg.SegmentIndex})");
            _wgcScreen.StartSegment(_seg.SegmentIndex);
            DebugLog.Write("[RecordAssist] _wgcScreen.StartSegment() returned");
        }

        private static void ReportSaveProgress(
            IProgress<RecordingSaveProgress> progress,
            string title,
            string message,
            int percent,
            int currentStep,
            int totalSteps,
            bool isIndeterminate = false)
        {
            if (progress == null)
                return;

            progress.Report(new RecordingSaveProgress
            {
                Title = title,
                Message = message,
                Percent = percent,
                CurrentStep = currentStep,
                TotalSteps = totalSteps,
                IsIndeterminate = isIndeterminate
            });
        }

        private void StopScreenSegment()
        {
            try { _wgcScreen?.StopSegment(); } catch { }
        }

        private async Task StartWebcamSegmentAsync()
        {
            if (string.IsNullOrWhiteSpace(_targets.WebcamName))
                return;

            _wgcWebcam ??= new MediaCaptureWebcamSegmentRecorder();

            if (_seg.SegmentIndex > 1)
            {
                DebugLog.Write("[RecordAssist] StartWebcamSegment resume -> ResumeAsync");
                await _wgcWebcam.ResumeAsync();
                DebugLog.Write("[RecordAssist] StartWebcamSegment resume OK");
                return;
            }

            string outFile = _paths.WebcamContinuous();
            DebugLog.Write($"[RecordAssist] StartWebcamSegment begin | webcam={_targets.WebcamName} | outFile={outFile}");

            int webcamWarmupMs = Math.Max(0, _runtimeOptions?.WebcamWarmupMilliseconds ?? 900);
            DebugLog.Write($"[RecordAssist] webcam logical warmup => {webcamWarmupMs}ms");

            await _wgcWebcam.PrepareAsync(_targets.WebcamName, outFile);
            await _wgcWebcam.StartAsync(_targets.WebcamName, outFile, webcamWarmupMs);

            DebugLog.Write("[RecordAssist] StartWebcamSegment OK");
        }

        private async Task StopWebcamSegmentAsync(bool releaseSession = false)
        {
            try
            {
                if (_wgcWebcam != null && _wgcWebcam.IsRecording)
                {
                    if (releaseSession)
                        await _wgcWebcam.StopAsync();
                    else
                        await _wgcWebcam.PauseAsync();
                }

                if (releaseSession && _wgcWebcam != null)
                {
                    await _wgcWebcam.ReleaseAsync();
                    _wgcWebcam = null;
                }
            }
            catch
            {
                if (releaseSession)
                    _wgcWebcam = null;
            }
        }

        private void StartMicSegment()
        {
            if (string.IsNullOrWhiteSpace(_targets.MicDeviceId))
                return;

            string outFile = _paths.MicSegment(_seg.SegmentIndex);
            (_micCapture, _micWriter) = MicCaptureService.Start(_targets.MicDeviceId, outFile);
        }

        private void StopMicSegment()
        {
            MicCaptureService.Stop(_micCapture, _micWriter);
            _micCapture = null;
            _micWriter = null;
        }

        private void StartLoopbackSegment()
        {
            if (string.IsNullOrWhiteSpace(_targets.LoopbackDeviceId))
                return;

            string outFile = _paths.SystemSegment(_seg.SegmentIndex);
            (_loopCapture, _loopWriter, _clock, _loopTick) =
                SystemLoopbackService.Start(_targets.LoopbackDeviceId, outFile);
        }

        private void StopLoopbackSegment()
        {
            SystemLoopbackService.Stop(_loopCapture, _loopWriter, _loopTick);
            _loopTick = null;
            _loopWriter = null;
            _loopCapture = null;
            _clock = new NessStudio.Models.AudioClockState();
        }

        private static async Task WaitForFileReadyAsync(string filePath, string label, int timeoutMs = 2500, int pollMs = 25)
        {
            if (string.IsNullOrWhiteSpace(filePath))
                return;

            var startedAt = Stopwatch.StartNew();

            while (startedAt.ElapsedMilliseconds < timeoutMs)
            {
                try
                {
                    if (!File.Exists(filePath))
                    {
                        await Task.Delay(pollMs).ConfigureAwait(false);
                        continue;
                    }

                    using var stream = new FileStream(
                        filePath,
                        FileMode.Open,
                        FileAccess.Read,
                        FileShare.Read);

                    if (stream.Length >= 0)
                    {
                        DebugLog.Write(
                            $"[RecordAssist] WaitForFileReadyAsync ready | " +
                            $"label={label} | file={filePath} | waited={startedAt.ElapsedMilliseconds}ms");
                        return;
                    }
                }
                catch
                {
                }

                await Task.Delay(pollMs).ConfigureAwait(false);
            }

            DebugLog.Write(
                $"[RecordAssist] WaitForFileReadyAsync timeout | " +
                $"label={label} | file={filePath} | waited={startedAt.ElapsedMilliseconds}ms");
        }

        private static async Task WaitForTrackFilesReadyAsync(IEnumerable<string> files, string label)
        {
            foreach (var file in files ?? Enumerable.Empty<string>())
            {
                if (string.IsNullOrWhiteSpace(file))
                    continue;

                await WaitForFileReadyAsync(file, label).ConfigureAwait(false);
            }
        }

        public async Task<string> StopAndFinalizeAsync(IProgress<RecordingSaveProgress> progress = null)
        {
            const int totalSteps = 6;

            var capturedPauseIntervals = new List<SessionManifest.PauseInterval>();
            ScreenTrackSnapshot screenSnapshot = null;

            DebugLog.Write("[RecordAssist] StopAndFinalizeAsync begin");
            ReportSaveProgress(progress, "Saving Recording...", "Preparing finalization...", 5, 1, totalSteps);

            if (_seg.IsRunning)
            {
                DebugLog.Write("[RecordAssist] stopping active segments...");
                ReportSaveProgress(progress, "Saving Recording...", "Finalizing screen capture...", 20, 2, totalSteps);

                try
                {
                    StopScreenSegment();

                    if (_wgcScreen != null)
                    {
                        capturedPauseIntervals = BuildPauseIntervals(_wgcScreen);
                        screenSnapshot = CaptureScreenSnapshot(_wgcScreen);
                    }

                    _wgcScreen?.ReleaseSession();
                    _wgcScreen = null;

                    DebugLog.Write("[RecordAssist] StopScreenSegment + ReleaseSession OK");
                }
                catch (Exception ex)
                {
                    DebugLog.Write("[RecordAssist] StopScreenSegment ERROR:\n" + ex);
                }

                ReportSaveProgress(progress, "Saving Recording...", "Finalizing webcam...", 35, 3, totalSteps);
                try
                {
                    await StopWebcamSegmentAsync(releaseSession: true);
                    DebugLog.Write("[RecordAssist] StopWebcamSegmentAsync OK");
                }
                catch (Exception ex)
                {
                    DebugLog.Write("[RecordAssist] StopWebcamSegmentAsync ERROR:\n" + ex);
                }

                ReportSaveProgress(progress, "Saving Recording...", "Finalizing microphone...", 50, 4, totalSteps);
                try
                {
                    StopMicSegment();
                    DebugLog.Write("[RecordAssist] StopMicSegment OK");
                }
                catch (Exception ex)
                {
                    DebugLog.Write("[RecordAssist] StopMicSegment ERROR:\n" + ex);
                }

                ReportSaveProgress(progress, "Saving Recording...", "Finalizing system audio...", 65, 5, totalSteps);
                try
                {
                    StopLoopbackSegment();
                    DebugLog.Write("[RecordAssist] StopLoopbackSegment OK");
                }
                catch (Exception ex)
                {
                    DebugLog.Write("[RecordAssist] StopLoopbackSegment ERROR:\n" + ex);
                }

                _seg.TryStop();
                DebugLog.Write("[RecordAssist] segment state -> stopped");
            }

            CleanupLegacyJoinArtifacts();

            var screenContinuousFile = _paths.ScreenContinuous();
            var screenParts = File.Exists(screenContinuousFile)
                ? new List<string> { screenContinuousFile }
                : _paths.Parts(_paths.ScreenPrefix, _paths.ScreenExt);

            var webcamContinuousFile = _paths.WebcamContinuous();
            var webcamParts = File.Exists(webcamContinuousFile)
                ? new List<string> { webcamContinuousFile }
                : _paths.Parts(_paths.WebcamPrefix, _paths.WebcamExt);
            var micParts = _paths.Parts(_paths.MicPrefix, _paths.MicExt);
            var sysParts = _paths.Parts(_paths.SystemPrefix, _paths.SystemExt);

            await WaitForTrackFilesReadyAsync(micParts, "mic").ConfigureAwait(false);
            await WaitForTrackFilesReadyAsync(sysParts, "system").ConfigureAwait(false);

            string screenPrimary = GetPrimaryTrackFile(screenParts);
            string webcamPrimary = GetPrimaryTrackFile(webcamParts);
            string micPrimary = GetPrimaryTrackFile(micParts);
            string sysPrimary = GetPrimaryTrackFile(sysParts);

            RecordingPerfProbe.Mark(
                "recording-audio-files-ready",
                $"micParts={micParts.Count} | sysParts={sysParts.Count}");

            DebugLog.Write("[RecordAssist] writing manifest inline...");
            WriteManifestNow(
                screenParts,
                webcamParts,
                micParts,
                sysParts,
                screenPrimary,
                webcamPrimary,
                micPrimary,
                sysPrimary,
                capturedPauseIntervals,
                screenSnapshot);

            DebugLog.Write(
                $"[RecordAssist] StopAndFinalizeAsync end => " +
                $"screen={screenPrimary}, webcam={webcamPrimary}, mic={micPrimary}, system={sysPrimary}");
            DebugLog.Flush();

            return screenPrimary
                ?? webcamPrimary
                ?? micPrimary
                ?? sysPrimary;
        }

        private static string GetPrimaryTrackFile(List<string> parts)
        {
            if (parts == null || parts.Count == 0)
                return null;

            return parts
                .Where(File.Exists)
                .Where(f =>
                {
                    try
                    {
                        return new FileInfo(f).Length > 0;
                    }
                    catch
                    {
                        return false;
                    }
                })
                .OrderBy(f => f, StringComparer.OrdinalIgnoreCase)
                .FirstOrDefault();
        }

        private SessionManifest.AudioTrack BuildAudioManifest(string primaryFile, List<string> parts)
        {
            var info = SessionManifestWriter.ReadAudioInfo(primaryFile);
            if (info == null)
                return null;

            var segments = new List<SessionManifest.SegmentEntry>();
            foreach (var p in parts ?? new List<string>())
            {
                var seg = SessionManifestWriter.BuildAudioSegment(p);
                if (seg != null)
                    segments.Add(seg);
            }

            info.Segments = segments;
            info.Duration = SessionManifestWriter.SumDurations(segments);
            return info;
        }

        private static List<SessionManifest.PauseInterval> BuildPauseIntervals(WgcScreenCapturePipe wgcScreen)
        {
            var result = new List<SessionManifest.PauseInterval>();
            if (wgcScreen == null)
                return result;

            foreach (var interval in wgcScreen.PauseIntervals)
            {
                result.Add(new SessionManifest.PauseInterval
                {
                    PauseHns = interval.PauseHns,
                    ResumeHns = interval.ResumeHns
                });
            }

            return result;
        }

        private static ScreenTrackSnapshot CaptureScreenSnapshot(WgcScreenCapturePipe wgcScreen)
        {
            if (wgcScreen == null)
                return null;

            return new ScreenTrackSnapshot
            {
                Width = wgcScreen.ScreenWidth,
                Height = wgcScreen.ScreenHeight,
                Fps = wgcScreen.ScreenFps,
                FrameCount = wgcScreen.ScreenFrameCount,
                StrideY = wgcScreen.ScreenStrideY,
                StrideUV = wgcScreen.ScreenStrideUV,
                PixelFormat = wgcScreen.ScreenPixelFormat,
                IsRawIntermediate = false
            };
        }

        private static string TryBuildScreenDuration(long frameCount, int fps)
        {
            if (frameCount <= 0 || fps <= 0)
                return "00:00:00";

            double seconds = frameCount / (double)fps;
            return SessionManifestWriter.FormatDuration(TimeSpan.FromSeconds(seconds));
        }

        private List<SessionManifest.SegmentEntry> BuildVideoSegments(List<string> parts)
        {
            var result = new List<SessionManifest.SegmentEntry>();

            foreach (var p in parts ?? new List<string>())
            {
                if (string.IsNullOrWhiteSpace(p) || !File.Exists(p))
                    continue;

                try
                {
                    long fileLength = new FileInfo(p).Length;
                    if (fileLength <= 0)
                    {
                        DebugLog.Write($"[RecordAssist] BuildVideoSegments -> skip zero-byte file {p}");
                        continue;
                    }
                }
                catch (Exception ex)
                {
                    DebugLog.Write($"[RecordAssist] BuildVideoSegments -> file length check failed {p}\n{ex}");
                    continue;
                }

                try
                {
                    DebugLog.Write($"[RecordAssist] BuildVideoSegments -> reading {p}");

                    var v = VideoInfoReader.ReadMp4Info(p);

                    result.Add(new SessionManifest.SegmentEntry
                    {
                        File = Path.GetFileName(p),
                        Duration = SessionManifestWriter.FormatDuration(v?.Duration ?? TimeSpan.Zero)
                    });

                    DebugLog.Write(
                        $"[RecordAssist] BuildVideoSegments -> done {p} | " +
                        $"duration={SessionManifestWriter.FormatDuration(v?.Duration ?? TimeSpan.Zero)}");
                }
                catch (Exception ex)
                {
                    DebugLog.Write($"[RecordAssist] BuildVideoSegments -> invalid/corrupt video skipped {p}\n{ex}");
                }
            }

            return result;
        }

        private void CleanupLegacyJoinArtifacts()
        {
            try
            {
                string[] trash =
                {
                    Path.Combine(_paths.BaseDir, "audio_mix_final.wav"),
                    Path.Combine(_paths.BaseDir, "screen_final_mux.mp4"),
                    Path.Combine(_paths.BaseDir, "webcam_final_mux.mp4"),
                    Path.Combine(_paths.BaseDir, "screen_final.mp4"),
                    Path.Combine(_paths.BaseDir, "webcam_final.mp4"),
                    Path.Combine(_paths.BaseDir, "mic_final.wav"),
                    Path.Combine(_paths.BaseDir, "system_final.wav"),
                    Path.Combine(_paths.BaseDir, "concat_screen.txt"),
                    Path.Combine(_paths.BaseDir, "concat_webcam.txt"),
                    Path.Combine(_paths.BaseDir, "concat_mic.txt"),
                    Path.Combine(_paths.BaseDir, "concat_system.txt")
                };

                foreach (var t in trash)
                {
                    try
                    {
                        if (File.Exists(t))
                            File.Delete(t);
                    }
                    catch
                    {
                    }
                }

                foreach (var f in Directory.EnumerateFiles(_paths.BaseDir, "*.tmp_*.mp4", SearchOption.TopDirectoryOnly))
                {
                    try { File.Delete(f); } catch { }
                }
            }
            catch (Exception ex)
            {
                DebugLog.Write("[RecordAssist] CleanupLegacyJoinArtifacts warning:\n" + ex);
            }
        }

        private void WriteManifestNow(
            List<string> screenParts,
            List<string> webcamParts,
            List<string> micParts,
            List<string> sysParts,
            string screenPrimary,
            string webcamPrimary,
            string micPrimary,
            string sysPrimary,
            List<SessionManifest.PauseInterval> pauseIntervals,
            ScreenTrackSnapshot screenSnapshot)
        {
            DebugLog.Write("[RecordAssist] WriteManifestNow begin");

            try
            {
                DebugLog.Write("[RecordAssist] manifest -> BuildVideoSegments(screen) begin");
                var screenSegments = BuildVideoSegments(screenParts);
                DebugLog.Write("[RecordAssist] manifest -> BuildVideoSegments(screen) end");

                DebugLog.Write("[RecordAssist] manifest -> BuildVideoSegments(webcam) begin");
                var webcamSegments = BuildVideoSegments(webcamParts);
                DebugLog.Write("[RecordAssist] manifest -> BuildVideoSegments(webcam) end");

                if (screenSnapshot != null && screenSegments.Count > 0)
                {
                    var screenDuration = TryBuildScreenDuration(screenSnapshot.FrameCount, screenSnapshot.Fps);
                    screenSegments[0].FrameCount = screenSnapshot.FrameCount;
                    screenSegments[0].Duration = screenDuration;
                }

                DebugLog.Write("[RecordAssist] manifest -> ReadMp4Info(screenPrimary) begin");
                var screenInfo = !string.IsNullOrWhiteSpace(screenPrimary) && File.Exists(screenPrimary)
                    ? VideoInfoReader.ReadMp4Info(screenPrimary)
                    : null;
                DebugLog.Write("[RecordAssist] manifest -> ReadMp4Info(screenPrimary) end");

                DebugLog.Write("[RecordAssist] manifest -> ReadMp4Info(webcamPrimary) begin");
                var webcamInfo = !string.IsNullOrWhiteSpace(webcamPrimary) && File.Exists(webcamPrimary)
                    ? VideoInfoReader.ReadMp4Info(webcamPrimary)
                    : null;
                DebugLog.Write("[RecordAssist] manifest -> ReadMp4Info(webcamPrimary) end");

                var manifest = new SessionManifest
                {
                    Version = 2,
                    SessionType = "nessmuxer-mkv",
                    SessionId = Guid.NewGuid().ToString("N"),
                    CreatedAt = DateTimeOffset.UtcNow,
                    BaseDir = _paths.BaseDir,
                    HasScreen = !string.IsNullOrWhiteSpace(screenPrimary),
                    HasWebcam = !string.IsNullOrWhiteSpace(webcamPrimary),
                    HasMic = !string.IsNullOrWhiteSpace(micPrimary),
                    HasSystemAudio = !string.IsNullOrWhiteSpace(sysPrimary),

                    Screen = string.IsNullOrWhiteSpace(screenPrimary) ? null : new SessionManifest.VideoTrack
                    {
                        File = Path.GetFileName(screenPrimary),
                        Duration = screenInfo?.Duration != null && screenInfo.Duration > TimeSpan.Zero
                            ? SessionManifestWriter.FormatDuration(screenInfo.Duration)
                            : TryBuildScreenDuration(screenSnapshot?.FrameCount ?? 0, screenSnapshot?.Fps ?? 30),
                        Segments = screenSegments,
                        Width = screenSnapshot?.Width ?? screenInfo?.Width ?? 0,
                        Height = screenSnapshot?.Height ?? screenInfo?.Height ?? 0,
                        Fps = screenSnapshot?.Fps ?? screenInfo?.Fps ?? 30,
                        ContainerKind = "mkv",
                        PixelFormat = screenSnapshot?.PixelFormat,
                        FrameCount = screenSnapshot?.FrameCount,
                        StrideY = screenSnapshot?.StrideY,
                        StrideUV = screenSnapshot?.StrideUV,
                        IsRawIntermediate = false,
                        HasDrawAreaCrop = _region?.CropGdi != null,
                        CropPx = _region?.CropGdi == null ? null : new SessionManifest.CropRect
                        {
                            X = _region.CropGdi.Value.X,
                            Y = _region.CropGdi.Value.Y,
                            W = _region.CropGdi.Value.Width,
                            H = _region.CropGdi.Value.Height
                        }
                    },

                    Webcam = string.IsNullOrWhiteSpace(webcamPrimary) ? null : new SessionManifest.VideoTrack
                    {
                        File = Path.GetFileName(webcamPrimary),
                        Duration = webcamSegments.Count > 0
                            ? SessionManifestWriter.SumDurations(webcamSegments)
                            : SessionManifestWriter.FormatDuration(webcamInfo?.Duration ?? TimeSpan.Zero),
                        Segments = webcamSegments,
                        Width = webcamInfo?.Width ?? 0,
                        Height = webcamInfo?.Height ?? 0,
                        Fps = webcamInfo?.Fps ?? 30,
                        ContainerKind = "mp4",
                        PixelFormat = null,
                        FrameCount = null,
                        StrideY = null,
                        StrideUV = null,
                        IsRawIntermediate = false
                    },

                    PauseIntervals = pauseIntervals,
                    Mic = BuildAudioManifest(micPrimary, micParts),
                    System = BuildAudioManifest(sysPrimary, sysParts)
                };

                SessionManifestWriter.WriteJson(_paths.BaseDir, manifest);
                DebugLog.Write("[RecordAssist] session manifest written");
            }
            catch (Exception ex)
            {
                DebugLog.Write("[RecordAssist] WriteManifestNow ERROR:\n" + ex);
            }

            DebugLog.Write("[RecordAssist] WriteManifestNow end");
        }

        public void Dispose()
        {
            try { _wgcScreen?.StopSegment(); } catch { }

            try
            {
                var screenToRelease = _wgcScreen;
                _wgcScreen = null;
                screenToRelease?.ReleaseSession();
            }
            catch
            {
            }

            try
            {
                StopWebcamSegmentAsync(releaseSession: true).GetAwaiter().GetResult();
            }
            catch
            {
            }

            StopMicSegment();
            StopLoopbackSegment();
            _wgcWebcam = null;
        }
    }
}
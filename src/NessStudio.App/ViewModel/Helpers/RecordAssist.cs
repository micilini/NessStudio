// Historical capture adapter, compiled only by the isolated baseline host.
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
        private WasapiTimelineRecorder? _micTrack;
        private WasapiTimelineRecorder? _systemTrack;
        private Exception? _finalizationFailure;
        private readonly NessStudio.Models.RecordingSegmentState _seg = new NessStudio.Models.RecordingSegmentState();
        private readonly RecordingRuntimeOptions _runtimeOptions;
        private readonly RecordingTimeline _timeline = new();
        /// <summary>Gets active time from the same legacy timeline used by all recorded tracks.</summary>
        public TimeSpan ActiveDuration => TimeSpan.FromTicks(_timeline.ElapsedHns);
        private DateTimeOffset? _sessionStartedAtUtc;
        private DateTimeOffset? _sessionStoppedAtUtc;
        private long? _sessionDurationMs;
        private List<SessionManifest.PauseInterval> _finalizedPauseIntervals = new List<SessionManifest.PauseInterval>();
        private ScreenTrackSnapshot _finalizedScreenSnapshot;
        private bool _tracksStoppedForFinalize;

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
            PhysicalRectangle? cropPx = null,
            RecordingRuntimeOptions runtimeOptions = null)
        {
            _paths = paths ?? throw new ArgumentNullException(nameof(paths));
            _targets = targets ?? throw new ArgumentNullException(nameof(targets));
            _runtimeOptions = runtimeOptions ?? new RecordingRuntimeOptions();
            _region = new ScreenRegion(targets.Screen, cropPx);
        }

        private void StartMasterSessionClock()
        {
            _sessionStartedAtUtc = DateTimeOffset.UtcNow;
            _sessionStoppedAtUtc = null;
            _sessionDurationMs = null;
            _timeline.Start();
        }

        private void PauseMasterSessionClock() => _timeline.Pause();
        private void ResumeMasterSessionClock() => _timeline.Resume();

        private void StopMasterSessionClock()
        {
            _timeline.Pause();
            _sessionStoppedAtUtc ??= DateTimeOffset.UtcNow;
            _sessionDurationMs = (_timeline.ElapsedHns + 5_000) / 10_000;
        }

        private bool ShouldCaptureScreen()
        {
            return _region != null && _region.ShouldCapture;
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
            if (_seg.IsRunning) return;
            _tracksStoppedForFinalize = false;
            _finalizationFailure = null;
            _finalizedPauseIntervals = new List<SessionManifest.PauseInterval>();
            _finalizedScreenSnapshot = null;
            try
            {
                // Open all devices before publishing the common recording epoch.
                if (ShouldCaptureScreen()) InitializeScreenSession();
                if (!string.IsNullOrWhiteSpace(_targets.MicDeviceId))
                {
                    _micTrack = new WasapiTimelineRecorder(_targets.MicDeviceId, false, _timeline);
                    await _micTrack.StartAsync(_paths.MicContinuous());
                }
                if (!string.IsNullOrWhiteSpace(_targets.LoopbackDeviceId))
                {
                    _systemTrack = new WasapiTimelineRecorder(_targets.LoopbackDeviceId, true, _timeline);
                    await _systemTrack.StartAsync(_paths.SystemContinuous());
                }
                _seg.Start();
                await StartWebcamSegmentAsync();
                StartMasterSessionClock();
                if (ShouldCaptureScreen()) StartScreenSegment();
            }
            catch
            {
                StopMasterSessionClock();
                try { await StopActiveTracksForFinalizationAsync(); } catch { }
                throw;
            }
        }

        public async Task PauseAsync()
        {
            if (!_seg.IsRunning || _seg.IsPaused) return;
            _seg.TryPause();
            PauseMasterSessionClock();
            if (ShouldCaptureScreen()) await Task.Run(StopScreenSegment);
            await StopWebcamSegmentAsync();
            // Audio clients keep running. Packet timestamps exclude paused samples.
        }

        public async Task ResumeAsync()
        {
            if (!_seg.IsRunning || !_seg.IsPaused) return;
            _seg.TryResume();
            if (!string.IsNullOrWhiteSpace(_targets.WebcamName)) await StartWebcamSegmentAsync();
            ResumeMasterSessionClock();
            if (ShouldCaptureScreen()) StartScreenSegment();
        }

        public async Task StopAsync()
        {
            if (!_seg.IsRunning && _tracksStoppedForFinalize)
                return;

            DebugLog.Write("[RecordAssist] StopAsync begin | immediate final stop requested");
            StopMasterSessionClock();
            await StopActiveTracksForFinalizationAsync().ConfigureAwait(false);
            DebugLog.Write("[RecordAssist] StopAsync end");
        }

        private async Task StopActiveTracksForFinalizationAsync()
        {
            if (_tracksStoppedForFinalize)
                return;

            _tracksStoppedForFinalize = true;
            var errors = new List<Exception>();
            _finalizedPauseIntervals = BuildPauseIntervals(_timeline);

            if (ShouldCaptureScreen())
            {
                try
                {
                    StopScreenSegment();

                    if (_wgcScreen != null)
                    {
                        _finalizedScreenSnapshot = CaptureScreenSnapshot(_wgcScreen);
                    }

                    _wgcScreen?.Dispose();
                    _wgcScreen = null;

                    DebugLog.Write("[RecordAssist] StopActiveTracksForFinalizationAsync -> screen stopped/released");
                }
                catch (Exception ex)
                {
                    DebugLog.Write("[RecordAssist] StopActiveTracksForFinalizationAsync -> screen ERROR:\n" + UserError.Details(ex));
                    errors.Add(ex);
                }
            }

            try
            {
                await StopWebcamSegmentAsync(releaseSession: true).ConfigureAwait(false);
                DebugLog.Write("[RecordAssist] StopActiveTracksForFinalizationAsync -> webcam stopped/released");
            }
            catch (Exception ex)
            {
                DebugLog.Write("[RecordAssist] StopActiveTracksForFinalizationAsync -> webcam ERROR:\n" + UserError.Details(ex));
                errors.Add(ex);
            }

            try
            {
                StopMicSegment(finalStop: true);
                DebugLog.Write("[RecordAssist] StopActiveTracksForFinalizationAsync -> mic stopped");
            }
            catch (Exception ex)
            {
                DebugLog.Write("[RecordAssist] StopActiveTracksForFinalizationAsync -> mic ERROR:\n" + UserError.Details(ex));
                errors.Add(ex);
            }

            try
            {
                StopLoopbackSegment(finalStop: true);
                DebugLog.Write("[RecordAssist] StopActiveTracksForFinalizationAsync -> system audio stopped");
            }
            catch (Exception ex)
            {
                DebugLog.Write("[RecordAssist] StopActiveTracksForFinalizationAsync -> system audio ERROR:\n" + UserError.Details(ex));
                errors.Add(ex);
            }

            _seg.TryStop();
            if (errors.Count > 0)
            {
                _finalizationFailure = new AggregateException("Recording tracks did not finalize successfully.", errors);
                throw _finalizationFailure;
            }
        }

        private void InitializeScreenSession()
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

                _wgcScreen.UseTimeline(_timeline);
                DebugLog.Write("[RecordAssist] Calling _wgcScreen.InitializeSession() on UI thread");
                _wgcScreen.InitializeSession();
                DebugLog.Write("[RecordAssist] _wgcScreen.InitializeSession() returned");
            }
            else
            {
                DebugLog.Write("[RecordAssist] WgcScreenCapturePipe session reused");
            }

        }

        private void StartScreenSegment() => _wgcScreen.StartSegment(_seg.SegmentIndex);

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
            _wgcScreen?.StopSegment();
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

        private void StopMicSegment(bool finalStop = false)
        {
            if (!finalStop) return;
            try { _micTrack?.Dispose(); }
            finally { _micTrack = null; }
        }

        private void StopLoopbackSegment(bool finalStop = false)
        {
            if (!finalStop) return;
            try { _systemTrack?.Dispose(); }
            finally { _systemTrack = null; }
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
                catch { }

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

            DebugLog.Write("[RecordAssist] StopAndFinalizeAsync begin");
            ReportSaveProgress(progress, "Saving Recording...", "Preparing finalization...", 5, 1, totalSteps);

            if (_seg.IsRunning || !_tracksStoppedForFinalize)
            {
                DebugLog.Write("[RecordAssist] StopAndFinalizeAsync -> stopping active tracks now");
                ReportSaveProgress(progress, "Saving Recording...", "Stopping active tracks...", 20, 2, totalSteps);
                StopMasterSessionClock();
                await StopActiveTracksForFinalizationAsync().ConfigureAwait(false);
                DebugLog.Write("[RecordAssist] StopAndFinalizeAsync -> active tracks stopped");
            }

            if (_finalizationFailure != null) throw _finalizationFailure;

            var capturedPauseIntervals = _finalizedPauseIntervals ?? new List<SessionManifest.PauseInterval>();
            ScreenTrackSnapshot screenSnapshot = _finalizedScreenSnapshot;

            CleanupLegacyJoinArtifacts();

            var screenContinuousFile = _paths.ScreenContinuous();
            var screenParts = File.Exists(screenContinuousFile)
                ? new List<string> { screenContinuousFile }
                : _paths.Parts(_paths.ScreenPrefix, _paths.ScreenExt);

            var webcamContinuousFile = _paths.WebcamContinuous();
            var webcamParts = File.Exists(webcamContinuousFile)
                ? new List<string> { webcamContinuousFile }
                : _paths.Parts(_paths.WebcamPrefix, _paths.WebcamExt);

            var micContinuous = _paths.MicContinuous();
            var micParts = File.Exists(micContinuous)
                ? new List<string> { micContinuous }
                : _paths.Parts(_paths.MicPrefix, _paths.MicExt);

            var sysContinuous = _paths.SystemContinuous();
            var sysParts = File.Exists(sysContinuous)
                ? new List<string> { sysContinuous }
                : _paths.Parts(_paths.SystemPrefix, _paths.SystemExt);

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
                    try { return new FileInfo(f).Length > 0; }
                    catch { return false; }
                })
                .OrderBy(f => f, StringComparer.OrdinalIgnoreCase)
                .FirstOrDefault();
        }

        private SessionManifest.AudioTrack BuildAudioManifest(string primaryFile, int offsetMs = 0)
        {
            var info = SessionManifestWriter.ReadAudioInfo(primaryFile);
            if (info == null)
                return null;

            if (offsetMs > 0)
                info.OffsetMs = offsetMs;

            return info;
        }

        private static List<SessionManifest.PauseInterval> BuildPauseIntervals(RecordingTimeline timeline)
        {
            var result = new List<SessionManifest.PauseInterval>();
            foreach (var interval in timeline.PauseIntervals)
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
                    try { if (File.Exists(t)) File.Delete(t); } catch { }
                }

                foreach (var f in Directory.EnumerateFiles(_paths.BaseDir, "*.tmp_*.mp4", SearchOption.TopDirectoryOnly))
                {
                    try { File.Delete(f); } catch { }
                }
            }
            catch (Exception ex)
            {
                DebugLog.Write("[RecordAssist] CleanupLegacyJoinArtifacts warning:\n" + UserError.Details(ex));
            }
        }

        private void WriteManifestNow(
            List<string> screenParts,
            List<string> webcamParts,
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
                    TimingMode = "qpc-active-v1",
                    SessionType = "nessmuxer-mkv",
                    SessionId = Guid.NewGuid().ToString("N"),
                    CreatedAt = DateTimeOffset.UtcNow,
                    BaseDir = _paths.BaseDir,
                    StartedAtUtc = _sessionStartedAtUtc,
                    StoppedAtUtc = _sessionStoppedAtUtc,
                    DurationMs = _sessionDurationMs,
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
                        Width = screenSnapshot?.Width ?? screenInfo?.Width ?? 0,
                        Height = screenSnapshot?.Height ?? screenInfo?.Height ?? 0,
                        Fps = screenSnapshot?.Fps ?? screenInfo?.Fps ?? 30,
                        ContainerKind = "mkv",
                        PixelFormat = screenSnapshot?.PixelFormat,
                        FrameCount = screenSnapshot?.FrameCount,
                        StrideY = screenSnapshot?.StrideY,
                        StrideUV = screenSnapshot?.StrideUV,
                        IsRawIntermediate = false,
                        HasDrawAreaCrop = _region?.Crop != null,
                        CropPx = _region?.Crop == null ? null : new SessionManifest.CropRect
                        {
                            X = _region.Crop.Value.X,
                            Y = _region.Crop.Value.Y,
                            W = _region.Crop.Value.Width,
                            H = _region.Crop.Value.Height
                        }
                    },

                    Webcam = string.IsNullOrWhiteSpace(webcamPrimary) ? null : new SessionManifest.VideoTrack
                    {
                        File = Path.GetFileName(webcamPrimary),
                        Duration = SessionManifestWriter.FormatDuration(webcamInfo?.Duration ?? TimeSpan.Zero),
                        Width = webcamInfo?.Width ?? 0,
                        Height = webcamInfo?.Height ?? 0,
                        Fps = webcamInfo?.Fps ?? 30,
                        ContainerKind = "mp4",
                        IsRawIntermediate = false
                    },

                    PauseIntervals = pauseIntervals,
                    Mic = BuildAudioManifest(micPrimary),
                    System = BuildAudioManifest(sysPrimary)
                };

                SessionManifestWriter.WriteJson(_paths.BaseDir, manifest);
                DebugLog.Write("[RecordAssist] session manifest written");
            }
            catch (Exception ex)
            {
                DebugLog.Write("[RecordAssist] WriteManifestNow ERROR:\n" + UserError.Details(ex));
            }

            DebugLog.Write("[RecordAssist] WriteManifestNow end");
        }

        public void Dispose()
        {
            _timeline.Pause();
            try { _wgcScreen?.StopSegment(); } catch { }

            try
            {
                var screenToRelease = _wgcScreen;
                _wgcScreen = null;
                screenToRelease?.Dispose();
            }
            catch { }

            try
            {
                StopWebcamSegmentAsync(releaseSession: true).GetAwaiter().GetResult();
            }
            catch { }

            StopMicSegment(finalStop: true);

            try { StopLoopbackSegment(finalStop: true); } catch { }

            _wgcWebcam = null;
        }
    }
}

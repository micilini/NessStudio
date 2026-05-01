using NessStudio.ViewModel.Helpers;
using System;
using System.Threading.Tasks;

namespace NessStudio.Recording.Windows
{
    public sealed class MediaCaptureWebcamSegmentRecorder : IDisposable
    {
        private readonly MediaCaptureWebcamSession _session = MediaCaptureWebcamSession.Shared;

        public bool IsPrepared => _session.IsPrepared;
        public bool IsRecording => _session.IsRecording;

        public async Task PrepareAsync(string webcamFriendlyName, string outputPath)
        {
            DebugLog.Write($"[Webcam] PrepareAsync begin | friendlyName={webcamFriendlyName} | output={outputPath}");

            await _session.EnsureInitializedAsync(webcamFriendlyName).ConfigureAwait(false);
            await _session.PrepareRecordingAsync(outputPath).ConfigureAwait(false);

            DebugLog.Write("[Webcam] PrepareAsync end");
        }

        public async Task StartAsync(string webcamFriendlyName, string outputPath, int warmupMilliseconds = 900)
        {
            if (IsRecording) return;

            int normalizedWarmupMs = Math.Max(0, warmupMilliseconds);
            DateTime requestedAtUtc = DateTime.UtcNow;

            DebugLog.Write($"[Webcam] StartAsync begin | friendlyName={webcamFriendlyName} | warmup={normalizedWarmupMs}ms | prepared={IsPrepared}");

            await PrepareAsync(webcamFriendlyName, outputPath).ConfigureAwait(false);
            await _session.StartPreparedRecordingAsync(normalizedWarmupMs).ConfigureAwait(false);

            double armedAfterMs = (DateTime.UtcNow - requestedAtUtc).TotalMilliseconds;
            DebugLog.Write($"[Webcam] StartAsync end | armedAfter={armedAfterMs:F0}ms | output={outputPath}");
        }

        public async Task PauseAsync()
        {
            if (!IsRecording) return;

            DebugLog.Write("[Webcam] PauseAsync begin");
            await _session.PauseRecordingAsync().ConfigureAwait(false);
            DebugLog.Write("[Webcam] PauseAsync end");
        }

        public async Task ResumeAsync()
        {
            if (IsRecording) return;

            DebugLog.Write("[Webcam] ResumeAsync begin");
            await _session.ResumeRecordingAsync().ConfigureAwait(false);
            DebugLog.Write("[Webcam] ResumeAsync end");
        }

        public async Task StopAsync()
        {
            if (!IsPrepared && !IsRecording)
                return;

            try
            {
                await _session.StopRecordingAsync(keepSessionAlive: true).ConfigureAwait(false);
            }
            catch (Exception ex)
            {
                DebugLog.Write("[Webcam] StopAsync warning:\n" + ex);
            }
        }

        public async Task ReleaseAsync()
        {
            await _session.ReleaseAsync().ConfigureAwait(false);
        }

        public void Dispose()
        {
            try
            {
                ReleaseAsync().GetAwaiter().GetResult();
            }
            catch
            {
            }
        }
    }
}
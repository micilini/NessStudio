using NAudio.CoreAudioApi;
using NAudio.Wave;
using System;
using System.IO;
using System.Linq;

namespace NessStudio.ViewModel.Helpers
{
    public static class MicCaptureService
    {
        public static (WasapiCapture cap, WaveFileWriter writer) Start(string deviceId, string outputPath)
        {
            if (string.IsNullOrWhiteSpace(deviceId) || string.IsNullOrWhiteSpace(outputPath))
                return (null, null);
            try { Directory.CreateDirectory(Path.GetDirectoryName(outputPath)); } catch { }
            var mm = new MMDeviceEnumerator()
                .EnumerateAudioEndPoints(DataFlow.Capture, DeviceState.Active)
                .FirstOrDefault(d => string.Equals(d.ID, deviceId, StringComparison.OrdinalIgnoreCase));
            if (mm == null) return (null, null);
            var cap = new WasapiCapture(mm);
            var writer = new WaveFileWriter(outputPath, cap.WaveFormat);
            cap.DataAvailable += (s, a) =>
            {
                try { writer?.Write(a.Buffer, 0, a.BytesRecorded); } catch { }
            };
            cap.RecordingStopped += (s, a) =>
            {
                DebugLog.Write($"[Mic] RecordingStopped | exception={(a?.Exception == null ? "null" : a.Exception.Message)}");
            };
            cap.StartRecording();
            return (cap, writer);
        }

        public static WasapiCapture Resume(string deviceId, WaveFileWriter writer, DateTime pausedAt)
        {
            if (string.IsNullOrWhiteSpace(deviceId) || writer == null)
                return null;
            var mm = new MMDeviceEnumerator()
                .EnumerateAudioEndPoints(DataFlow.Capture, DeviceState.Active)
                .FirstOrDefault(d => string.Equals(d.ID, deviceId, StringComparison.OrdinalIgnoreCase));
            if (mm == null) return null;
            var cap = new WasapiCapture(mm);
            var wf = cap.WaveFormat;
            double pausedSeconds = (DateTime.UtcNow - pausedAt).TotalSeconds;
            if (pausedSeconds > 0)
            {
                long silenceBytes = (long)(wf.AverageBytesPerSecond * pausedSeconds);
                silenceBytes -= silenceBytes % wf.BlockAlign;
                if (silenceBytes > 0)
                {
                    try { writer.Write(new byte[silenceBytes], 0, (int)silenceBytes); } catch { }
                    DebugLog.Write($"[Mic] Resume silence | pausedMs={(int)(pausedSeconds * 1000)} bytes={silenceBytes}");
                }
            }
            cap.DataAvailable += (s, a) =>
            {
                try { writer?.Write(a.Buffer, 0, a.BytesRecorded); } catch { }
            };
            cap.RecordingStopped += (s, a) =>
            {
                DebugLog.Write($"[Mic] RecordingStopped | exception={(a?.Exception == null ? "null" : a.Exception.Message)}");
            };
            cap.StartRecording();
            return cap;
        }

        public static DateTime Pause(WasapiCapture cap)
        {
            var pausedAt = DateTime.UtcNow;
            try { cap?.StopRecording(); } catch { }
            try { System.Threading.Thread.Sleep(80); } catch { }
            try { cap?.Dispose(); } catch { }
            return pausedAt;
        }

        public static void Stop(WasapiCapture cap, WaveFileWriter writer)
        {
            if (cap == null && writer == null)
                return;
            try { cap?.StopRecording(); } catch { }
            try { System.Threading.Thread.Sleep(80); } catch { }
            try { writer?.Flush(); } catch { }
            try { writer?.Dispose(); } catch { }
            try { cap?.Dispose(); } catch { }
        }
    }
}
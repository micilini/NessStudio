using NAudio.CoreAudioApi;
using NAudio.Wave;
using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Text;
using System.Threading.Tasks;
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
                try
                {
                    writer?.Write(a.Buffer, 0, a.BytesRecorded);
                }
                catch
                {
                }
            };
            cap.RecordingStopped += (s, a) =>
            {
                DebugLog.Write($"[Mic] RecordingStopped | exception={(a?.Exception == null ? "null" : a.Exception.Message)}");
            };
            cap.StartRecording();
            return (cap, writer);
        }
        public static void Stop(WasapiCapture cap, WaveFileWriter writer)
        {
            if (cap == null && writer == null)
                return;
            try
            {
                if (cap != null)
                    cap.StopRecording();
            }
            catch
            {
            }
            try { writer?.Flush(); } catch { }
            try { writer?.Dispose(); } catch { }
            try
            {
                if (cap != null)
                    cap.Dispose();
            }
            catch
            {
            }
        }
    }
}
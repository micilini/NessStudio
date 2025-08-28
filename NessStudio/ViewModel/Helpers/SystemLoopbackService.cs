using System;
using System.Linq;
using NAudio.CoreAudioApi;
using NAudio.Wave;
using NessStudio.Models;

namespace NessStudio.ViewModel.Helpers
{
    public static class SystemLoopbackService
    {
        public static (WasapiLoopbackCapture cap, WaveFileWriter writer, AudioClockState clock, System.Timers.Timer tick)
            Start(string deviceId, string outputPath, int preSilenceMs = 500, int tickMs = 20)
        {
            if (string.IsNullOrWhiteSpace(deviceId) || string.IsNullOrWhiteSpace(outputPath))
                return (null, null, null, null);

            var mm = new MMDeviceEnumerator()
                .EnumerateAudioEndPoints(DataFlow.Render, DeviceState.Active)
                .FirstOrDefault(d => string.Equals(d.ID, deviceId, StringComparison.OrdinalIgnoreCase));
            if (mm == null) return (null, null, null, null);

            var cap = new WasapiLoopbackCapture(mm);
            var writer = new WaveFileWriter(outputPath, cap.WaveFormat);
            var clock = new AudioClockState();

            var wf = cap.WaveFormat;

            clock.Start(wf);

            int preBytes = clock.AlignedBytesForMs(preSilenceMs);
            if (preBytes > 0)
            {
                writer.Write(new byte[preBytes], 0, preBytes);
                clock.AddWritten(preBytes);
            }

            cap.DataAvailable += (s, a) =>
            {
                writer.Write(a.Buffer, 0, a.BytesRecorded);
                clock.AddWritten(a.BytesRecorded);
            };

            var tick = new System.Timers.Timer(tickMs) { AutoReset = true };
            tick.Elapsed += (s, e) =>
            {
                if (writer == null || cap == null) return;

                int maxPerTick = wf.AverageBytesPerSecond / 10;
                int writeNow = clock.MissingBytes(DateTime.UtcNow, maxPerTick);
                if (writeNow <= 0) return;

                var silence = new byte[writeNow];
                writer.Write(silence, 0, writeNow);
                clock.AddWritten(writeNow);
            };
            tick.Start();

            cap.RecordingStopped += (s, a) =>
            {
                try { tick.Stop(); } catch { }
                try { tick.Dispose(); } catch { }

                try { writer?.Flush(); } catch { }
                try { writer?.Dispose(); } catch { }
                try { cap?.Dispose(); } catch { }

                clock.Stop();
            };

            cap.StartRecording();
            return (cap, writer, clock, tick);
        }

        public static void Stop(WasapiLoopbackCapture cap, WaveFileWriter writer, System.Timers.Timer tick)
        {
            try { tick?.Stop(); } catch { }
            try { tick?.Dispose(); } catch { }

            try { cap?.StopRecording(); } catch { }
            try { writer?.Flush(); } catch { }
            try { writer?.Dispose(); } catch { }
            try { cap?.Dispose(); } catch { }
        }
    }
}
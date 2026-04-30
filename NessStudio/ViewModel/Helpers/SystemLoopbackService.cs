using System;
using System.Linq;
using System.Threading;
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

            if (mm == null)
                return (null, null, null, null);

            var cap = new WasapiLoopbackCapture(mm);
            var writer = new WaveFileWriter(outputPath, cap.WaveFormat);
            var clock = new AudioClockState();
            var wf = cap.WaveFormat;
            System.Timers.Timer tick = null;
            int cleanupState = 0;

            void CleanupCore(string origin)
            {
                if (Interlocked.Exchange(ref cleanupState, 1) != 0)
                    return;

                DebugLog.Write($"[SystemLoopback] CleanupCore begin | origin={origin}");

                try { tick?.Stop(); } catch { }
                try { tick?.Dispose(); } catch { }

                try { writer?.Flush(); } catch { }
                try { writer?.Dispose(); } catch { }

                try { cap?.Dispose(); } catch { }

                try { clock.Stop(); } catch { }

                DebugLog.Write($"[SystemLoopback] CleanupCore end | origin={origin}");
            }

            clock.Start(wf);

            int preBytes = clock.AlignedBytesForMs(preSilenceMs);
            if (preBytes > 0)
            {
                writer.Write(new byte[preBytes], 0, preBytes);
                clock.AddWritten(preBytes);
            }

            cap.DataAvailable += (s, a) =>
            {
                try
                {
                    writer.Write(a.Buffer, 0, a.BytesRecorded);
                    clock.AddWritten(a.BytesRecorded);
                }
                catch (Exception ex)
                {
                    DebugLog.Write("[SystemLoopback] DataAvailable ERROR:\n" + ex);
                }
            };

            tick = new System.Timers.Timer(tickMs) { AutoReset = true };
            tick.Elapsed += (s, e) =>
            {
                try
                {
                    if (Volatile.Read(ref cleanupState) != 0)
                        return;

                    int maxPerTick = wf.AverageBytesPerSecond / 10;
                    int writeNow = clock.MissingBytes(DateTime.UtcNow, maxPerTick);
                    if (writeNow <= 0)
                        return;

                    var silence = new byte[writeNow];
                    writer.Write(silence, 0, writeNow);
                    clock.AddWritten(writeNow);
                }
                catch (Exception ex)
                {
                    DebugLog.Write("[SystemLoopback] Tick ERROR:\n" + ex);
                }
            };

            cap.RecordingStopped += (s, a) =>
            {
                DebugLog.Write(
                    $"[SystemLoopback] RecordingStopped | " +
                    $"exception={(a?.Exception == null ? "null" : a.Exception.Message)}");

                CleanupCore("RecordingStopped");
            };

            tick.Start();
            cap.StartRecording();

            DebugLog.Write($"[SystemLoopback] Start OK | output={outputPath}");

            return (cap, writer, clock, tick);
        }

        public static void Stop(WasapiLoopbackCapture cap, WaveFileWriter writer, System.Timers.Timer tick)
        {
            DebugLog.Write("[SystemLoopback] Stop begin");

            try { tick?.Stop(); } catch { }
            try { tick?.Dispose(); } catch { }

            try { writer?.Flush(); } catch { }
            try { writer?.Dispose(); } catch { }

            try
            {
                cap?.StopRecording();
            }
            catch (Exception ex)
            {
                DebugLog.Write("[SystemLoopback] StopRecording warning:\n" + ex);
            }

            try { System.Threading.Thread.Sleep(80); } catch { }

            DebugLog.Write("[SystemLoopback] Stop end");
        }
    }
}
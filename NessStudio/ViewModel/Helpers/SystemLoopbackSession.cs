using NAudio.CoreAudioApi;
using NAudio.Wave;
using NessStudio.Models;
using System;
using System.Linq;
using System.Threading;

namespace NessStudio.ViewModel.Helpers
{
    public sealed class SystemLoopbackSession : IDisposable
    {
        private readonly string _deviceId;
        private WasapiLoopbackCapture _cap;
        private WaveFileWriter _writer;
        private AudioClockState _clock;
        private System.Timers.Timer _tick;
        private volatile int _paused = 1;
        private DateTime _pausedAt;
        private bool _disposed;
        private const int TickMs = 20;
        private const int PreSilenceMs = 500;

        public SystemLoopbackSession(string deviceId)
        {
            _deviceId = deviceId;
        }

        public void Start(string outputPath)
        {
            if (_writer != null) return;
            var mm = FindDevice();
            if (mm == null) return;

            _cap = new WasapiLoopbackCapture(mm);
            _writer = new WaveFileWriter(outputPath, _cap.WaveFormat);
            _clock = new AudioClockState();
            _clock.Start(_cap.WaveFormat);

            int preBytes = _clock.AlignedBytesForMs(PreSilenceMs);
            if (preBytes > 0) { _writer.Write(new byte[preBytes], 0, preBytes); _clock.AddWritten(preBytes); }

            _cap.DataAvailable += OnDataAvailable;
            _cap.RecordingStopped += (s, a) =>
                DebugLog.Write($"[SystemLoopbackSession] RecordingStopped | {a?.Exception?.Message ?? "null"}");

            _tick = new System.Timers.Timer(TickMs) { AutoReset = true };
            _tick.Elapsed += OnTick;

            Interlocked.Exchange(ref _paused, 0);
            _tick.Start();
            _cap.StartRecording();

            DebugLog.Write($"[SystemLoopbackSession] Start OK | output={outputPath}");
        }

        public void Pause()
        {
            Interlocked.Exchange(ref _paused, 1);
            _pausedAt = DateTime.UtcNow;
            _tick?.Stop();
            DebugLog.Write("[SystemLoopbackSession] Pause | cap kept running, data ignored");
        }

        public void Resume()
        {
            if (_writer == null || _cap == null) return;
            var wf = _cap.WaveFormat;
            double pausedSeconds = (DateTime.UtcNow - _pausedAt).TotalSeconds;
            if (pausedSeconds > 0.01)
            {
                long silenceBytes = (long)(wf.AverageBytesPerSecond * pausedSeconds);
                silenceBytes -= silenceBytes % wf.BlockAlign;
                if (silenceBytes > 0)
                {
                    try { _writer.Write(new byte[silenceBytes], 0, (int)silenceBytes); } catch { }
                    DebugLog.Write($"[SystemLoopbackSession] Resume silence | pausedMs={(int)(pausedSeconds * 1000)} bytes={silenceBytes}");
                }
            }
            _clock.Start(wf);
            Interlocked.Exchange(ref _paused, 0);
            _tick?.Start();
            DebugLog.Write("[SystemLoopbackSession] Resumed");
        }

        public void Stop()
        {
            Interlocked.Exchange(ref _paused, 1);
            try { _tick?.Stop(); _tick?.Dispose(); } catch { }
            try { _cap?.StopRecording(); } catch { }
            Thread.Sleep(80);
            try { _writer?.Flush(); _writer?.Dispose(); } catch { }
            try { _cap?.Dispose(); } catch { }
            _clock?.Stop();
            _tick = null; _writer = null; _cap = null; _clock = null;
            DebugLog.Write("[SystemLoopbackSession] Stop OK");
        }

        private void OnDataAvailable(object s, WaveInEventArgs a)
        {
            if (_paused != 0) return;
            try { _writer?.Write(a.Buffer, 0, a.BytesRecorded); _clock?.AddWritten(a.BytesRecorded); } catch { }
        }

        private void OnTick(object s, System.Timers.ElapsedEventArgs e)
        {
            if (_paused != 0) return;
            try
            {
                var wf = _cap?.WaveFormat;
                if (wf == null) return;
                int maxPerTick = wf.AverageBytesPerSecond / 10;
                int writeNow = _clock?.MissingBytes(DateTime.UtcNow, maxPerTick) ?? 0;
                if (writeNow <= 0) return;
                _writer?.Write(new byte[writeNow], 0, writeNow);
                _clock?.AddWritten(writeNow);
            }
            catch { }
        }

        private MMDevice FindDevice() =>
            new MMDeviceEnumerator()
                .EnumerateAudioEndPoints(DataFlow.Render, DeviceState.Active)
                .FirstOrDefault(d => string.Equals(d.ID, _deviceId, StringComparison.OrdinalIgnoreCase));

        public void Dispose()
        {
            if (_disposed) return;
            _disposed = true;
            Stop();
        }
    }
}
using System.Buffers.Binary;
using System.Diagnostics;
using NAudio.CoreAudioApi;
using NAudio.Wave;
using NessStudio.ViewModel.Helpers;

namespace NessStudio.Presentation;

/// <summary>Monitors selected audio sources without applying recording gain or writing audio files.</summary>
public sealed class AudioLevelMonitor : IAsyncDisposable
{
    private readonly object _gate = new();
    private readonly CancellationTokenSource _stop = new();
    private readonly Task _worker;
    private Request _microphone = new(null, new Signal());
    private Request _systemAudio = new(null, new Signal());
    private bool _disposed;

    public AudioLevelMonitor() => _worker = Task.Run(RunAsync);

    /// <summary>Changes monitored endpoints; null disables that channel and immediately clears its meter.</summary>
    public void Configure(string? microphoneDeviceId, string? systemAudioDeviceId)
    {
        lock (_gate)
        {
            if (_disposed) return;
            _microphone = Change(_microphone, microphoneDeviceId);
            _systemAudio = Change(_systemAudio, systemAudioDeviceId);
        }
    }

    /// <summary>Returns the latest input peaks as percentages; silent or disabled sources return zero.</summary>
    public (double Microphone, double SystemAudio) ReadLevels()
    {
        lock (_gate)
            return (_microphone.Signal.Read(), _systemAudio.Signal.Read());
    }

    /// <summary>Stops capture and releases endpoints without blocking the presentation thread.</summary>
    public async ValueTask DisposeAsync()
    {
        lock (_gate)
        {
            if (!_disposed)
            {
                _disposed = true;
                _microphone = new(null, new Signal());
                _systemAudio = new(null, new Signal());
                _stop.Cancel();
            }
        }
        await _worker.ConfigureAwait(false);
    }

    private static Request Change(Request previous, string? deviceId)
    {
        deviceId = string.IsNullOrWhiteSpace(deviceId) ? null : deviceId;
        return string.Equals(previous.DeviceId, deviceId, StringComparison.OrdinalIgnoreCase)
            ? previous : new(deviceId, new Signal());
    }

    private async Task RunAsync()
    {
        using var microphone = new ChannelSlot(loopback: false);
        using var systemAudio = new ChannelSlot(loopback: true);
        try
        {
            while (!_stop.IsCancellationRequested)
            {
                Request microphoneRequest, systemRequest;
                lock (_gate) { microphoneRequest = _microphone; systemRequest = _systemAudio; }
                microphone.Update(microphoneRequest);
                systemAudio.Update(systemRequest);
                await Task.Delay(100, _stop.Token).ConfigureAwait(false);
            }
        }
        catch (OperationCanceledException) when (_stop.IsCancellationRequested) { }
    }

    private sealed record Request(string? DeviceId, Signal Signal);

    private sealed class Signal
    {
        private readonly object _sync = new();
        private double _latestPeak, _peakSinceRead;
        private long _timestamp;

        internal void Update(double peak)
        {
            lock (_sync)
            {
                _latestPeak = peak;
                _peakSinceRead = Math.Max(_peakSinceRead, peak);
                _timestamp = Stopwatch.GetTimestamp();
            }
        }

        internal double Read()
        {
            lock (_sync)
            {
                // Loopback can stop delivering packets entirely when playback becomes silent.
                double peak = _timestamp != 0 && Stopwatch.GetElapsedTime(_timestamp).TotalMilliseconds <= 250
                    ? Math.Max(_latestPeak, _peakSinceRead) : 0;
                _peakSinceRead = 0;
                return peak * 100;
            }
        }

        internal void Reset()
        {
            lock (_sync) { _latestPeak = _peakSinceRead = 0; _timestamp = 0; }
        }
    }

    private sealed class ChannelSlot(bool loopback) : IDisposable
    {
        private Request? _request;
        private Capture? _capture;
        private long _retryAfter;
        private bool _failureReported;

        internal void Update(Request request)
        {
            if (!ReferenceEquals(_request, request))
            {
                Dispose();
                _request = request;
                _retryAfter = 0;
                _failureReported = false;
            }
            if (request.DeviceId is null) return;
            if (_capture?.Stopped == true)
            {
                ReportFailure(_capture.Error);
                Dispose();
                _retryAfter = Environment.TickCount64 + 3000;
            }
            if (_capture is not null || Environment.TickCount64 < _retryAfter) return;
            try { _capture = new Capture(request, loopback); }
            catch (Exception error)
            {
                request.Signal.Reset();
                ReportFailure(error);
                _retryAfter = Environment.TickCount64 + 3000;
            }
        }

        private void ReportFailure(Exception? error)
        {
            if (_failureReported) return;
            _failureReported = true;
            DebugLog.Write($"[AudioMeter] {(loopback ? "System audio" : "Microphone")} monitoring unavailable; retrying: {error?.Message ?? "Capture stopped."}");
        }

        public void Dispose()
        {
            _capture?.Dispose();
            _capture = null;
            _request?.Signal.Reset();
        }
    }

    private sealed class Capture : IDisposable
    {
        private readonly Request _request;
        private readonly MMDevice _device;
        private readonly WasapiCapture _capture;
        private readonly WaveFormat _format;
        private int _stopped;
        internal bool Stopped => Volatile.Read(ref _stopped) != 0;
        internal Exception? Error { get; private set; }

        internal Capture(Request request, bool loopback)
        {
            _request = request;
            using var enumerator = new MMDeviceEnumerator();
            _device = enumerator.GetDevice(request.DeviceId!);
            WasapiCapture? capture = null;
            try
            {
                // Shared mode allows the native recorder to use the same endpoint concurrently.
                capture = loopback ? new WasapiLoopbackCapture(_device) : new WasapiCapture(_device, false, 50);
                capture.ShareMode = AudioClientShareMode.Shared;
                _format = capture.WaveFormat is WaveFormatExtensible extended
                    ? extended.ToStandardWaveFormat() : capture.WaveFormat;
                if (!Supported(_format))
                    throw new NotSupportedException($"Unsupported audio meter format: {_format}.");
                _capture = capture;
                _capture.DataAvailable += OnDataAvailable;
                _capture.RecordingStopped += OnStopped;
                _capture.StartRecording();
            }
            catch
            {
                if (capture is not null)
                {
                    capture.DataAvailable -= OnDataAvailable;
                    capture.RecordingStopped -= OnStopped;
                    try { capture.Dispose(); } catch { }
                }
                _device.Dispose();
                throw;
            }
        }

        private void OnDataAvailable(object? sender, WaveInEventArgs args) =>
            _request.Signal.Update(MeasurePeak(args.Buffer, args.BytesRecorded, _format));

        private void OnStopped(object? sender, StoppedEventArgs args)
        {
            Error = args.Exception;
            Volatile.Write(ref _stopped, 1);
            _request.Signal.Reset();
        }

        public void Dispose()
        {
            _capture.DataAvailable -= OnDataAvailable;
            _capture.RecordingStopped -= OnStopped;
            try { _capture.StopRecording(); } catch { }
            try { _capture.Dispose(); } catch { }
            try { _device.Dispose(); } catch { }
            _request.Signal.Reset();
        }
    }

    private static bool Supported(WaveFormat format) => format.Encoding switch
    {
        WaveFormatEncoding.IeeeFloat => format.BitsPerSample is 32 or 64,
        WaveFormatEncoding.Pcm => format.BitsPerSample is 8 or 16 or 24 or 32,
        _ => false
    };

    private static double MeasurePeak(byte[] buffer, int count, WaveFormat format)
    {
        int bytesPerSample = format.BitsPerSample / 8;
        if (bytesPerSample == 0 || !Supported(format)) return 0;
        int length = Math.Clamp(count, 0, buffer.Length);
        double peak = 0;
        for (int offset = 0; offset <= length - bytesPerSample; offset += bytesPerSample)
        {
            var bytes = buffer.AsSpan(offset, bytesPerSample);
            double sample = format.Encoding == WaveFormatEncoding.IeeeFloat
                ? format.BitsPerSample == 32 ? BinaryPrimitives.ReadSingleLittleEndian(bytes)
                    : BinaryPrimitives.ReadDoubleLittleEndian(bytes)
                : format.BitsPerSample switch
                {
                    8 => (bytes[0] - 128) / 128.0,
                    16 => BinaryPrimitives.ReadInt16LittleEndian(bytes) / 32768.0,
                    24 => ((bytes[0] << 8 | bytes[1] << 16 | bytes[2] << 24) >> 8) / 8388608.0,
                    32 => BinaryPrimitives.ReadInt32LittleEndian(bytes) / 2147483648.0,
                    _ => 0
                };
            if (double.IsFinite(sample)) peak = Math.Max(peak, Math.Abs(sample));
        }
        return Math.Min(1, peak);
    }
}

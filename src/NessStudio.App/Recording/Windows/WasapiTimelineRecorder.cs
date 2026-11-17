using System;
using System.IO;
using System.Runtime.ExceptionServices;
using System.Runtime.InteropServices;
using System.Threading;
using System.Threading.Tasks;
using NAudio.CoreAudioApi;
using NAudio.Wave;

namespace NessStudio.Recording.Windows;

/// <summary>Writes real WASAPI packets at their QPC positions on the shared active timeline.</summary>
/// <remarks>The worker exclusively owns all COM objects and the WAV writer. Silence is filled
/// only for confirmed timestamp gaps or at finalization, never by a competing timer.</remarks>
internal sealed class WasapiTimelineRecorder : IDisposable
{
    private readonly string _deviceId;
    private readonly bool _loopback;
    private readonly RecordingTimeline _timeline;
    private readonly CancellationTokenSource _stop = new();
    private readonly TaskCompletionSource _ready = new(TaskCreationOptions.RunContinuationsAsynchronously);
    private Thread? _worker;
    private Exception? _failure;
    private bool _disposed;
    private long _packetAgeTotalHns, _packetAgeSamples, _packetFramesTotal, _streamLatencyHns;

    internal WasapiTimelineRecorder(string deviceId, bool loopback, RecordingTimeline timeline)
    {
        _deviceId = deviceId;
        _loopback = loopback;
        _timeline = timeline;
    }

    internal Task StartAsync(string path)
    {
        if (_worker != null) throw new InvalidOperationException("Audio worker already started.");
        _worker = new Thread(() => Capture(path)) { IsBackground = true, Name = _loopback ? "NessStudio-Loopback" : "NessStudio-Microphone" };
        _worker.Start();
        return _ready.Task.WaitAsync(TimeSpan.FromSeconds(10));
    }

    private void Capture(string path)
    {
        try
        {
            using var enumerator = new MMDeviceEnumerator();
            using var device = enumerator.GetDevice(_deviceId);
            using var client = device.AudioClient;
            var format = client.MixFormat;
            client.Initialize(AudioClientShareMode.Shared, _loopback ? AudioClientStreamFlags.Loopback : AudioClientStreamFlags.None,
                1_000_000, 0, format, Guid.Empty);
            var capture = client.AudioCaptureClient; // AudioClient owns and disposes the service.
            _streamLatencyHns = client.StreamLatency;
            Directory.CreateDirectory(Path.GetDirectoryName(path)!);
            using var writer = new WaveFileWriter(path, format.AsStandardWaveFormat());
            var pcm = new byte[checked(client.BufferSize * format.BlockAlign)];
            var silence = new byte[8192 * format.BlockAlign];
            long written = 0;
            client.Start();
            try
            {
                _ready.TrySetResult();
                while (true)
                {
                    while (capture.GetNextPacketSize() > 0)
                    {
                        IntPtr buffer = capture.GetBuffer(out int frames, out var flags, out _, out long qpc);
                        try
                        {
                            _packetAgeTotalHns += RecordingTimeline.GetQpcHns() - qpc;
                            _packetAgeSamples++;
                            _packetFramesTotal += frames;
                            if ((flags & AudioClientBufferFlags.TimestampError) != 0)
                                throw new InvalidOperationException("WASAPI supplied an invalid packet timestamp; synchronization cannot be guaranteed.");
                            int bytes = checked(frames * format.BlockAlign);
                            if (bytes > pcm.Length) throw new InvalidDataException("WASAPI packet exceeds the negotiated buffer.");
                            if ((flags & AudioClientBufferFlags.Silent) != 0) Array.Clear(pcm, 0, bytes);
                            else Marshal.Copy(buffer, pcm, 0, bytes);
                            foreach (var part in _timeline.SliceAudio(qpc, frames, format.SampleRate))
                            {
                                FillSilence(writer, silence, part.DestinationFrame - written, format.BlockAlign, ref written);
                                int skip = (int)Math.Min(part.Frames, Math.Max(0, written - part.DestinationFrame));
                                int count = part.Frames - skip;
                                writer.Write(pcm, (part.SourceFrame + skip) * format.BlockAlign, count * format.BlockAlign);
                                written += count;
                            }
                        }
                        finally { capture.ReleaseBuffer(frames); }
                    }
                    if (_stop.IsCancellationRequested) break;
                    _stop.Token.WaitHandle.WaitOne(5);
                }
                long target = (_timeline.ElapsedHns * format.SampleRate + TimeSpan.TicksPerSecond / 2) / TimeSpan.TicksPerSecond;
                FillSilence(writer, silence, target - written, format.BlockAlign, ref written);
                if (written > target + 1) throw new InvalidDataException("Audio extends beyond the stopped recording timeline.");
            }
            finally { client.Stop(); }
        }
        catch (Exception error)
        {
            _failure = error;
            _ready.TrySetException(error);
        }
    }

    private static void FillSilence(WaveFileWriter writer, byte[] silence, long frames, int align, ref long written)
    {
        while (frames > 0)
        {
            int chunk = (int)Math.Min(frames, silence.Length / align);
            writer.Write(silence, 0, chunk * align);
            frames -= chunk;
            written += chunk;
        }
    }

    internal void Stop()
    {
        _stop.Cancel();
        if (_worker != null && !_worker.Join(TimeSpan.FromSeconds(5)))
            throw new TimeoutException("WASAPI worker did not finish; its resources remain owned by the worker.");
        if (_failure != null) ExceptionDispatchInfo.Capture(_failure).Throw();
    }

    public void Dispose()
    {
        if (_disposed) return;
        try { Stop(); }
        finally
        {
            if (_worker == null || !_worker.IsAlive)
            {
                _disposed = true;
                _stop.Dispose();
            }
        }
    }
}

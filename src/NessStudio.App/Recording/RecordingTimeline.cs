using System;
using System.Collections.Generic;
using System.Diagnostics;

namespace NessStudio.Recording;

/// <summary>Maps QPC timestamps to one active recording timeline, excluding paused intervals.</summary>
/// <remarks>All times are 100 ns units. Device timestamps remain in the QPC domain, never UTC.</remarks>
internal sealed class RecordingTimeline
{
    private readonly object _sync = new();
    private readonly Func<long> _now;
    private readonly List<Segment> _segments = new();
    private long _duration;
    private long _epoch;
    private bool _running;

    internal RecordingTimeline(Func<long>? now = null) => _now = now ?? GetQpcHns;

    internal static long GetQpcHns()
    {
        long ticks = Stopwatch.GetTimestamp();
        return ticks / Stopwatch.Frequency * TimeSpan.TicksPerSecond
            + ticks % Stopwatch.Frequency * TimeSpan.TicksPerSecond / Stopwatch.Frequency;
    }

    internal bool IsRunning { get { lock (_sync) return _running; } }
    internal bool HasStarted { get { lock (_sync) return _segments.Count > 0; } }
    internal long ElapsedHns { get { lock (_sync) return _running ? _duration + _now() - _segments[^1].Start : _duration; } }

    internal void Start()
    {
        lock (_sync)
        {
            _segments.Clear();
            _duration = 0;
            _epoch = _now();
            _segments.Add(new(_epoch, long.MaxValue, 0));
            _running = true;
        }
    }

    internal void Pause()
    {
        lock (_sync)
        {
            if (!_running) return;
            var last = _segments[^1];
            long end = Math.Max(last.Start, _now());
            _segments[^1] = last with { End = end };
            _duration += end - last.Start;
            _running = false;
        }
    }

    internal void Resume()
    {
        lock (_sync)
        {
            if (_running || _segments.Count == 0) return;
            _segments.Add(new(_now(), long.MaxValue, _duration));
            _running = true;
        }
    }

    internal long CurrentSegmentStartHns { get { lock (_sync) return _segments.Count == 0 ? 0 : _segments[^1].Start; } }

    internal long QpcAtActiveTime(long activeHns)
    {
        lock (_sync)
        {
            for (int i = _segments.Count - 1; i >= 0; i--)
                if (activeHns >= _segments[i].ActiveStart)
                    return _segments[i].Start + activeHns - _segments[i].ActiveStart;
            throw new InvalidOperationException("Recording timeline has not started.");
        }
    }

    internal (long PauseHns, long ResumeHns)[] PauseIntervals
    {
        get
        {
            lock (_sync)
            {
                var result = new (long, long)[Math.Max(0, _segments.Count - 1)];
                for (int i = 1; i < _segments.Count; i++)
                    result[i - 1] = (_segments[i - 1].End - _epoch, _segments[i].Start - _epoch);
                return result;
            }
        }
    }

    /// <summary>Clips a packet against active intervals, including packets delivered after a pause.</summary>
    internal List<AudioSlice> SliceAudio(long packetQpc, int frames, int sampleRate)
    {
        lock (_sync)
        {
            var result = new List<AudioSlice>(1);
            foreach (var segment in _segments)
            {
                long from = Math.Max(0, DivideCeiling((segment.Start - packetQpc) * sampleRate, TimeSpan.TicksPerSecond));
                long to = segment.End == long.MaxValue ? frames : Math.Min(frames, DivideCeiling((segment.End - packetQpc) * sampleRate, TimeSpan.TicksPerSecond));
                if (from >= to) continue;
                long activeTime = segment.ActiveStart + packetQpc - segment.Start;
                long destination = Math.Max(0, (activeTime * sampleRate + from * TimeSpan.TicksPerSecond + TimeSpan.TicksPerSecond / 2) / TimeSpan.TicksPerSecond);
                result.Add(new((int)from, (int)(to - from), destination));
            }
            return result;
        }
    }

    internal static long FrameTimestamp(long index, int fps) => checked(index * TimeSpan.TicksPerSecond) / fps;
    internal static long RequiredFrames(long durationHns, int fps) => DivideCeiling(checked(durationHns * fps), TimeSpan.TicksPerSecond);
    private static long DivideCeiling(long value, long divisor) => value / divisor + (value % divisor > 0 ? 1 : 0);
    private readonly record struct Segment(long Start, long End, long ActiveStart);
    internal readonly record struct AudioSlice(int SourceFrame, int Frames, long DestinationFrame);
}

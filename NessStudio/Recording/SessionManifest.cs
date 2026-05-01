using System;
using System.Collections.Generic;

namespace NessStudio.Recording
{
    public sealed class SessionManifest
    {
        public int Version { get; set; } = 2;
        public string SessionType { get; set; } = "nessmuxer-mkv";
        public string SessionId { get; set; }
        public DateTimeOffset CreatedAt { get; set; }

        public string BaseDir { get; set; }

        public DateTimeOffset? StartedAtUtc { get; set; }
        public DateTimeOffset? StoppedAtUtc { get; set; }
        public long? DurationMs { get; set; }
        public bool HasScreen { get; set; }
        public bool HasWebcam { get; set; }
        public bool HasMic { get; set; }
        public bool HasSystemAudio { get; set; }

        public VideoTrack Screen { get; set; }
        public VideoTrack Webcam { get; set; }
        public AudioTrack Mic { get; set; }
        public AudioTrack System { get; set; }

        public List<PauseInterval> PauseIntervals { get; set; } = new();

        public sealed class PauseInterval
        {
            public long PauseHns { get; set; }
            public long ResumeHns { get; set; }
        }

        public sealed class VideoTrack
        {
            public string File { get; set; }
            public string Duration { get; set; }
            public int Width { get; set; }
            public int Height { get; set; }
            public int Fps { get; set; }
            public string ContainerKind { get; set; }
            public string PixelFormat { get; set; }
            public long? FrameCount { get; set; }
            public int? StrideY { get; set; }
            public int? StrideUV { get; set; }
            public bool IsRawIntermediate { get; set; }
            public bool HasDrawAreaCrop { get; set; }
            public CropRect CropPx { get; set; }
        }

        public sealed class AudioTrack
        {
            public string File { get; set; }
            public string Duration { get; set; }
            public int SampleRate { get; set; }
            public int Channels { get; set; }
            public int BitsPerSample { get; set; }
            public long? OffsetMs { get; set; }
        }

        // Mantida para compatibilidade com SessionManifestWriter.BuildAudioSegment / SumDurations
        public sealed class SegmentEntry
        {
            public string File { get; set; }
            public string Duration { get; set; }
            public long? FrameCount { get; set; }
        }

        public sealed class CropRect
        {
            public int X { get; set; }
            public int Y { get; set; }
            public int W { get; set; }
            public int H { get; set; }
        }
    }
}
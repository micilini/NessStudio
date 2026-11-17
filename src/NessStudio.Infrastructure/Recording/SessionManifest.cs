using System;
using System.Collections.Generic;
using System.Text.Json;
using System.Text.Json.Serialization;

namespace NessStudio.Recording
{
    public sealed class SessionManifest
    {
        public int Version { get; set; } = 2;
        /// <summary>Identifies the v3 project schema; session.manifest.json remains an equivalent alias.</summary>
        public string? Schema { get; set; }
        public string? ProjectId { get; set; }
        public string? EngineVersion { get; set; }
        public string State { get; set; } = "Completed";
        public string? InterruptionCause { get; set; }
        public string? FinalizationResult { get; set; }
        public string? Thumbnail { get; set; }
        /// <summary>Describes actual retained assets. In v3 these entries are authoritative over the v2 track projections.</summary>
        public List<ProjectAsset> Assets { get; set; } = new();
        public List<string> Errors { get; set; } = new();
        public bool Recoverable { get; set; }
        [JsonExtensionData]
        public Dictionary<string, JsonElement>? UnknownFields { get; set; }
        public string SessionType { get; set; } = "nessmuxer-mkv";
        /// <summary>Gets or sets the optional timing contract; absent in earlier v2 projects.</summary>
        /// <remarks>qpc-active-v1 means assets already exclude pauses and share the QPC epoch.
        /// PauseIntervals then describe excluded wall-time intervals relative to that epoch.</remarks>
        public string? TimingMode { get; set; }
        /// <summary>Gets or sets the native stage selection for integrated screen/audio recordings.</summary>
        public Engines.RecordingSelection? NativeSelection { get; set; }
        /// <summary>Gets or sets the finalized primary asset; independent tracks remain available for editing.</summary>
        public string? PrimaryAsset { get; set; }
        /// <summary>Gets or sets the explicit retained asset mode; older projects retain separate sources.</summary>
        public string RecordingMode { get; set; } = "SeparateSources";
        /// <summary>Gets or sets the original immutable scene shared with the native compositor and exporter.</summary>
        public Engines.RecordingScene? Scene { get; set; }
        /// <summary>Gets or sets the bounded history of applied native scene revisions.</summary>
        public System.Text.Json.JsonElement? SceneHistory { get; set; }
        /// <summary>Gets or sets whether an independently identified program asset was finalized.</summary>
        public bool HasProgram { get; set; }
        /// <summary>Gets or sets the composed program track without relabeling it as a raw screen or camera.</summary>
        public VideoTrack? Program { get; set; }
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
            [JsonExtensionData] public Dictionary<string, JsonElement>? UnknownFields { get; set; }
        }

        public sealed class AudioTrack
        {
            public string File { get; set; }
            public string Duration { get; set; }
            public int SampleRate { get; set; }
            public int Channels { get; set; }
            public int BitsPerSample { get; set; }
            public long? OffsetMs { get; set; }
            [JsonExtensionData] public Dictionary<string, JsonElement>? UnknownFields { get; set; }
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

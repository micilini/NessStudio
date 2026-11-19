using System.Globalization;
using System.Text.Json;
using NessStudio.Models;

namespace NessStudio.Recording;

/// <summary>Validates complete native screen/audio v1 assets and projects only the metadata required by existing projects.</summary>
internal static class NativeSessionManifestReader
{
    private static readonly JsonSerializerOptions OutputOptions = new() { WriteIndented = true };

    internal static SessionManifest Read(string path, string directory, RecordingTargets targets, bool camera = false, bool program = false)
    {
        using var document = ProjectMetadataStore.ReadDocument(directory, Path.GetFileName(path))
            ?? throw new FileNotFoundException("Native recording metadata was not found.", path);
        var root = document.RootElement;
        bool audioOnly = !targets.AnyVideo;
        if (root.GetProperty("version").GetInt32() != 1 || !root.GetProperty("mediaComplete").GetBoolean() ||
            !root.GetProperty("finalized").GetBoolean() || root.GetProperty("firstError").ValueKind != JsonValueKind.Null ||
            (audioOnly ? root.GetProperty("kind").GetString() != "audio-only" :
                root.GetProperty("codec").GetString() != "h264" || root.GetProperty("container").GetString() != "matroska"))
            throw new InvalidDataException("The native manifest is unsupported or describes incomplete media.");
        string? screen = audioOnly ? null : Asset(root.GetProperty("outputPath").GetString(), directory, program ? "program.mkv" : camera ? "webcam.mkv" : "screen.mkv");
        long frames = audioOnly ? 0 : root.GetProperty("frames").GetInt64(), duration = root.GetProperty("durationHns").GetInt64();
        if (duration <= 0 || !audioOnly && (frames <= 0 || frames != root.GetProperty("packets").GetInt64() ||
            new FileInfo(screen!).Length != root.GetProperty("bytes").GetInt64()))
            throw new InvalidDataException("Native frame count, duration or finalized asset size is invalid.");
        var encoder = audioOnly ? default : root.GetProperty("encoder");
        int fps = audioOnly ? 0 : encoder.GetProperty("fpsNum").GetInt32();
        if (!audioOnly && (encoder.GetProperty("fpsDen").GetInt32() != 1 ||
            !Engines.NativeRecorderEngineFactory.NativeCapabilities.SupportedFrameRates.Contains(fps)))
            throw new InvalidDataException("The native recording cadence is unsupported by this project adapter.");
        string formatted = TimeSpan.FromTicks(duration).ToString("c", CultureInfo.InvariantCulture);
        var manifest = new SessionManifest
        {
            SessionType = "libnessstudio-mkv", TimingMode = "qpc-active-v1",
            SessionId = root.GetProperty("sessionId").GetUInt64().ToString(CultureInfo.InvariantCulture), BaseDir = directory,
            DurationMs = duration / TimeSpan.TicksPerMillisecond, HasScreen = !audioOnly && !camera && !program, HasWebcam = camera, HasProgram = program
        };
        if (!audioOnly)
        {
            var video = new SessionManifest.VideoTrack
            {
                File = program ? "program.mkv" : camera ? "webcam.mkv" : "screen.mkv", Duration = formatted, Width = encoder.GetProperty("width").GetInt32(),
                Height = encoder.GetProperty("height").GetInt32(), Fps = fps, ContainerKind = "matroska", PixelFormat = "h264",
                FrameCount = frames, IsRawIntermediate = false,
                // Cropping and pause removal were already performed by the native engine.
                HasDrawAreaCrop = false
            };
            if (program) manifest.Program = video; else if (camera) manifest.Webcam = video; else manifest.Screen = video;
        }
        var tracks = root.GetProperty("audio");
        if (tracks.ValueKind != JsonValueKind.Array || tracks.GetArrayLength() != (targets.CaptureMic ? 1 : 0) + (targets.CaptureSystem ? 1 : 0))
            throw new InvalidDataException("The finalized audio tracks do not match the selected sources.");
        foreach (var track in tracks.EnumerateArray())
        {
            string kind = track.GetProperty("kind").GetString()!;
            bool mic = kind == "microphone";
            if (kind is not ("microphone" or "loopback") || mic && manifest.HasMic || !mic && manifest.HasSystemAudio ||
                track.GetProperty("endpointId").GetString() != (mic ? targets.MicDeviceId : targets.LoopbackDeviceId) ||
                !track.GetProperty("finalized").GetBoolean() || track.GetProperty("outputRate").GetInt32() != 48000 ||
                track.GetProperty("outputChannels").GetInt32() != 2 || track.GetProperty("outputFormat").GetString() != "pcm_s16le_rf64")
                throw new InvalidDataException("The finalized audio identity or format is invalid.");
            string filename = mic ? "mic.wav" : "system.wav";
            string audioPath = Asset(track.GetProperty("path").GetString(), directory, filename);
            long samples = track.GetProperty("outputSamples").GetInt64();
            if (samples <= 0 || new FileInfo(audioPath).Length != checked(80 + samples * 4) ||
                Math.Abs(track.GetProperty("durationHns").GetInt64() - duration) > TimeSpan.TicksPerSecond / (audioOnly ? 30 : fps) + 1)
                throw new InvalidDataException("Finalized WAV length or duration differs from the native timeline.");
            var audio = new SessionManifest.AudioTrack { File = filename, Duration = TimeSpan.FromTicks(track.GetProperty("durationHns").GetInt64()).ToString("c", CultureInfo.InvariantCulture),
                SampleRate = 48000, Channels = 2, BitsPerSample = 16, OffsetMs = 0 };
            if (mic) { manifest.HasMic = true; manifest.Mic = audio; }
            else { manifest.HasSystemAudio = true; manifest.System = audio; }
        }
        return manifest;
    }
    private static string Asset(string? path, string directory, string expected)
    {
        string full = Path.GetFullPath(path ?? throw new InvalidDataException("Missing native asset path."));
        if (!string.Equals(full, Path.Combine(directory, expected), StringComparison.OrdinalIgnoreCase) || !File.Exists(full))
            throw new InvalidDataException("Native assets must be finalized files in the selected session directory.");
        return full;
    }
    internal static void WriteAtomic<T>(string path, T value)
    {
        string pending = path + ".pending";
        using (var output = new FileStream(pending, FileMode.CreateNew, FileAccess.Write, FileShare.None))
        {
            JsonSerializer.Serialize(output, value, OutputOptions);
            output.Flush(flushToDisk: true);
        }
        File.Move(pending, path, overwrite: false);
    }
}

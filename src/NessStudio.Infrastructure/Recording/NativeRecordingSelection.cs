using System.Text.Json;

namespace NessStudio.Recording.Engines;

/// <summary>Maps managed requests and reports the stages actually selected by native preparation.</summary>
internal static class NativeRecordingSelection
{
    internal static string ModeName(RecordingExecutionMode mode) => mode switch
    {
        RecordingExecutionMode.Automatic => "Auto",
        RecordingExecutionMode.RequireGpuResidentHardware => "RequireGpuResidentHardware",
        RecordingExecutionMode.FullSoftware => "FullSoftware",
        RecordingExecutionMode.GpuWithSoftwareEncoding => "GpuWithSoftwareEncoding",
        _ => throw new ArgumentOutOfRangeException(nameof(mode))
    };

    internal static string EncoderName(RecordingExecutionMode mode) => mode switch
    {
        RecordingExecutionMode.Automatic or RecordingExecutionMode.RequireGpuResidentHardware => "Auto",
        RecordingExecutionMode.FullSoftware or RecordingExecutionMode.GpuWithSoftwareEncoding => "X264",
        _ => throw new ArgumentOutOfRangeException(nameof(mode))
    };

    internal static bool CameraMatches(JsonElement device, string identity) =>
        string.Equals(device.GetProperty("id").GetString(), identity, StringComparison.OrdinalIgnoreCase) ||
        device.TryGetProperty("aliases", out var aliases) && aliases.EnumerateArray().Any(alias =>
            string.Equals(alias.GetString(), identity, StringComparison.OrdinalIgnoreCase));

    internal static RecordingExecutionMode EffectiveMode(JsonElement snapshot)
    {
        var metrics = snapshot.GetProperty("metrics");
        bool gpu = false, cpuEncoder = false;
        Inspect(metrics);
        if (metrics.TryGetProperty("cameraTrack", out var camera)) Inspect(camera);
        if (metrics.TryGetProperty("programTrack", out var program)) Inspect(program);
        return gpu ? cpuEncoder ? RecordingExecutionMode.GpuWithSoftwareEncoding
                                : RecordingExecutionMode.RequireGpuResidentHardware
                   : RecordingExecutionMode.FullSoftware;

        void Inspect(JsonElement track)
        {
            if (!track.TryGetProperty("encoderSelection", out var selection))
                throw new InvalidDataException("Native video preparation did not report a track's selected execution mode.");
            switch (selection.GetProperty("effectiveMode").GetString())
            {
                case "RequireGpuResidentHardware": gpu = true; break;
                case "GpuWithSoftwareEncoding": gpu = true; cpuEncoder = true; break;
                case "FullSoftware": cpuEncoder = true; break;
                default: throw new InvalidDataException("Native preparation reported an unknown execution mode.");
            }
        }
    }
}

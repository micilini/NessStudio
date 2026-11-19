namespace NessStudio.Recording.Engines;

/// <summary>Compression quality, independent of the selected capture area.</summary>
public enum RecordingQualityPreset
{
    /// <summary>Predictable bitrate and smaller recordings.</summary>
    Balanced,
    /// <summary>High quality H.264, with bitrate determined by picture complexity.</summary>
    High,
    /// <summary>Retains more detail at the cost of larger files.</summary>
    Maximum
}

/// <summary>Shared quality choices and source-sized video configuration.</summary>
public static class RecordingQualityPresets
{
    /// <summary>Returns a supported quality value, preserving high quality by default.</summary>
    public static RecordingQualityPreset Normalize(RecordingQualityPreset preset, RecordingExecutionMode mode) =>
        Enum.IsDefined(preset) ? preset : RecordingQualityPreset.High;

    /// <summary>Gets the user-facing compression label.</summary>
    public static string Label(RecordingQualityPreset preset) => preset switch
    {
        RecordingQualityPreset.Balanced => "Balanced · Smaller files",
        RecordingQualityPreset.Maximum => "Maximum · Larger files",
        _ => "High quality (recommended)"
    };

    /// <summary>Preserves source geometry up to Full HD. Small areas are never enlarged; odd edges are padded.</summary>
    public static RecordingVideoOptions Video(RecordingQualityPreset preset, int fps, int width, int height)
    {
        double scale = Math.Min(1, Math.Min(1920d / width, 1080d / height));
        int outputWidth = Math.Max(2, ((int)Math.Round(width * scale) + 1) & ~1);
        int outputHeight = Math.Max(2, ((int)Math.Round(height * scale) + 1) & ~1);
        long reference = preset == RecordingQualityPreset.Balanced ? 12_000_000 : 24_000_000;
        long bitrate = reference * outputWidth * outputHeight / (1920 * 1080) * fps / 30;
        return new(outputWidth, outputHeight, (int)Math.Clamp(bitrate, 100_000, 100_000_000));
    }
}

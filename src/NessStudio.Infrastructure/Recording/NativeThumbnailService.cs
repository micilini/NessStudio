using System.Text.Json;
using NessStudio.Interop.Native;

namespace NessStudio.Recording;

/// <summary>Creates project preview images through a cancellable native software job.</summary>
/// <param name="applicationDirectory">The package containing libnessstudio and its explicit FFmpeg tool.</param>
public sealed class NativeThumbnailService(string applicationDirectory)
{
    /// <summary>Decodes one real video frame to a new PNG of at most 640 by 360 pixels.</summary>
    /// <param name="inputPath">The existing local video asset.</param>
    /// <param name="outputPath">A new PNG path; existing files are preserved.</param>
    /// <param name="seek">The preferred timestamp. A clip ending earlier uses its first frame.</param>
    /// <param name="cancellationToken">Cancels the supervised child and awaits its cleanup.</param>
    /// <returns>The native completion receipt as an independent JSON value.</returns>
    public async Task<JsonElement> CreateAsync(string inputPath, string outputPath, TimeSpan seek,
        CancellationToken cancellationToken = default)
    {
        ArgumentException.ThrowIfNullOrWhiteSpace(inputPath);
        ArgumentException.ThrowIfNullOrWhiteSpace(outputPath);
        if (seek < TimeSpan.Zero || seek > TimeSpan.FromHours(24))
            throw new ArgumentOutOfRangeException(nameof(seek));
        cancellationToken.ThrowIfCancellationRequested();
        string package = Path.GetFullPath(applicationDirectory);
        string input = Path.GetFullPath(inputPath), output = Path.GetFullPath(outputPath);
        using var engine = NativeEngine.Open(package, "NessStudio.ProjectThumbnail");
        await using var job = engine.CreateJob(JsonSerializer.SerializeToElement(new
        {
            version = 1, type = "thumbnail", profile = "thumbnail-png", inputPath = input, outputPath = output,
            seekHns = seek.Ticks, ffmpegPath = Path.Combine(package, "Native", "FFmpeg", "ffmpeg.exe")
        }));
        using var result = await job.RunAsync(cancellationToken).ConfigureAwait(false);
        var root = result.RootElement;
        if (root.GetProperty("state").GetInt32() == 104)
            throw new OperationCanceledException("Native thumbnail creation was cancelled.", cancellationToken);
        if (root.GetProperty("state").GetInt32() != 105)
            throw new InvalidDataException("Native thumbnail creation failed: " + root.GetRawText());
        if (!string.Equals(root.GetProperty("outputPath").GetString(), output, StringComparison.OrdinalIgnoreCase) || !File.Exists(output))
            throw new InvalidDataException("Native thumbnail publication returned an unexpected asset.");
        return root.Clone();
    }
}

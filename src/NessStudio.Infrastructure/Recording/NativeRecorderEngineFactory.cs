using NessStudio.Diagnostics;
using NessStudio.Models;

namespace NessStudio.Recording.Engines;

/// <summary>Creates the qualified native screen/audio adapter without opening capture sources.</summary>
public sealed class NativeRecorderEngineFactory(string applicationDirectory, IDiagnosticLog? log = null) : IRecorderEngineFactory
{
    internal static readonly RecordingCapabilities NativeCapabilities = new("libnessstudio", true, true,
        Array.AsReadOnly(new[] { 24, 25, 30, 48, 50, 60 }));
    /// <inheritdoc />
    public RecordingCapabilities Capabilities => NativeCapabilities;

    /// <inheritdoc />
    public IRecordingService Create(RecordingRequest request)
    {
        ArgumentNullException.ThrowIfNull(request);
        ArgumentNullException.ThrowIfNull(request.Targets);
        ArgumentNullException.ThrowIfNull(request.RuntimeOptions);
        ArgumentException.ThrowIfNullOrWhiteSpace(request.OutputDirectory);
        if (!Enum.IsDefined(request.OutputMode)) throw new ArgumentOutOfRangeException(nameof(request));
        if (!request.Targets.AnyVideo && request.OutputMode != RecordingOutputMode.SeparateSources) throw new NotSupportedException("Program modes require a video source.");
        if (!Enum.IsDefined(request.ExecutionMode)) throw new ArgumentOutOfRangeException(nameof(request));
        if (!request.Targets.IsValid(out var selectionError)) throw new ArgumentException(selectionError, nameof(request));
        if (request.Targets.AnyVideo && !NativeCapabilities.SupportedFrameRates.Contains(request.RuntimeOptions.RecordingFps))
            throw new NotSupportedException("The requested recording cadence is unsupported.");
        if (request.Scene is { } scene && (scene.FpsNum != request.RuntimeOptions.RecordingFps || scene.FpsDen != 1))
            throw new ArgumentException("Scene and recording cadence must agree before preparation.", nameof(request));
        if (request.Video is { } video && (video.Width < 2 || video.Height < 2 || video.Width % 2 != 0 || video.Height % 2 != 0 ||
            video.BitrateBps is < 10000 or > 100000000 || video.Width > 1920 || video.Height > 1080 ||
            request.ExecutionMode == RecordingExecutionMode.FullSoftware && (video.Width > 1280 || video.Height > 720)))
            throw new NotSupportedException("The requested video size/bitrate is outside the qualified recording preset.");
        var directory = Path.GetFullPath(request.OutputDirectory);
        if (!Directory.Exists(directory)) throw new DirectoryNotFoundException(directory);
        foreach (string file in NativeRecordingService.OwnedAssetNames)
            if (File.Exists(Path.Combine(directory, file))) throw new IOException("Recording assets must be new: " + file);
        // RuntimeOptions is mutable for legacy callers. Capture its values at the boundary.
        var snapshot = request with { OutputDirectory = directory, RuntimeOptions = new RecordingRuntimeOptions
        {
            RecordingFps = request.RuntimeOptions.RecordingFps, CountdownSeconds = request.RuntimeOptions.CountdownSeconds,
            ScreenWarmupMilliseconds = request.RuntimeOptions.ScreenWarmupMilliseconds,
            WebcamWarmupMilliseconds = request.RuntimeOptions.WebcamWarmupMilliseconds,
            ThumbnailCaptureOffsetSeconds = request.RuntimeOptions.ThumbnailCaptureOffsetSeconds
        } };
        (log ?? NullDiagnosticLog.Instance).Write($"[RecorderFactory] implementation=libnessstudio requested={request.ExecutionMode} screen={request.Targets.Screen?.DeviceName} camera={request.Targets.WebcamName} audio={request.Targets.AnyAudio}");
        return new NativeRecordingService(snapshot, Path.GetFullPath(applicationDirectory), log ?? NullDiagnosticLog.Instance);
    }
}

using NessStudio.Models;
using NessStudio.ViewModel.Helpers;

namespace NessStudio.Recording.Engines;

/// <summary>Adapts UI-free requests to the preserved WGC/WASAPI/MediaCapture backend.</summary>
public sealed class LegacyRecorderEngineFactory : IRecorderEngineFactory
{
    private static readonly RecordingCapabilities LegacyCapabilities = new("legacy-wgc-cpu-readback", false, false,
        Array.AsReadOnly(new[] { 24, 25, 30, 48, 50, 60 }));

    /// <inheritdoc />
    public RecordingCapabilities Capabilities => LegacyCapabilities;

    /// <inheritdoc />
    public IRecordingService Create(RecordingRequest request)
    {
        ArgumentNullException.ThrowIfNull(request);
        if (request.ExecutionMode != RecordingExecutionMode.Automatic)
            throw new NotSupportedException("A strict native execution mode cannot select the legacy engine.");
        ArgumentNullException.ThrowIfNull(request.Targets);
        if ((request.Targets.MicDeviceId is not null && request.MicrophoneGain != 1) ||
            (request.Targets.LoopbackDeviceId is not null && request.SystemAudioGain != 1))
            throw new NotSupportedException("Recording audio levels require the native recording engine.");
        ArgumentNullException.ThrowIfNull(request.RuntimeOptions);
        ArgumentException.ThrowIfNullOrWhiteSpace(request.OutputDirectory);
        if (request.Targets.Screen is { } monitor &&
            (string.IsNullOrWhiteSpace(monitor.DeviceName) || monitor.Bounds.Width <= 0 || monitor.Bounds.Height <= 0))
            throw new ArgumentException("A monitor requires an identity and positive physical bounds.", nameof(request));
        if (request.Crop is { } crop && (crop.Width <= 0 || crop.Height <= 0))
            throw new ArgumentException("A crop requires positive physical bounds.", nameof(request));
        if (!request.Targets.IsValid(out var error)) throw new ArgumentException(error, nameof(request));
        if (!Capabilities.SupportedFrameRates.Contains(request.RuntimeOptions.RecordingFps))
            throw new NotSupportedException("The legacy backend does not support the requested frame rate.");
        var options = request.RuntimeOptions;
        var snapshot = new RecordingRuntimeOptions
        {
            RecordingFps = options.RecordingFps, CountdownSeconds = options.CountdownSeconds,
            ScreenWarmupMilliseconds = options.ScreenWarmupMilliseconds, WebcamWarmupMilliseconds = options.WebcamWarmupMilliseconds,
            ThumbnailCaptureOffsetSeconds = options.ThumbnailCaptureOffsetSeconds
        };
        var backend = new RecordAssist(new RecordingOutputPaths(request.OutputDirectory), request.Targets, request.Crop, snapshot);
        return new LegacyRecordingService(backend, Capabilities, request.OutputDirectory);
    }
}

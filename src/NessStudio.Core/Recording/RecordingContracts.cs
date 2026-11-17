using NessStudio.Models;

namespace NessStudio.Recording.Engines;

/// <summary>Describes the media request accepted by a managed recording service.</summary>
/// <param name="OutputDirectory">The new session asset directory.</param>
/// <param name="Targets">The selected source identities.</param>
/// <param name="Crop">The optional physical desktop crop.</param>
/// <param name="RuntimeOptions">The legacy-compatible settings.</param>
/// <param name="ExecutionMode">The requested execution policy, resolved before capture.</param>
/// <param name="Video">The optional explicit output preset, validated before opening sources.</param>
/// <param name="Camera">The optional explicit native camera format. A missing format is resolved before opening a recording.</param>
/// <param name="OutputMode">The requested program and independent source assets.</param>
/// <param name="Scene">The optional fixed canvas and initial composition.</param>
public sealed record RecordingRequest(string OutputDirectory, RecordingTargets Targets,
    PhysicalRectangle? Crop, RecordingRuntimeOptions RuntimeOptions,
    RecordingExecutionMode ExecutionMode = RecordingExecutionMode.Automatic, RecordingVideoOptions? Video = null,
    RecordingCameraOptions? Camera = null, RecordingOutputMode OutputMode = RecordingOutputMode.SeparateSources,
    RecordingScene? Scene = null);

/// <summary>Identifies the exact advertised input format independently of the encoded output.</summary>
/// <param name="DeviceId">The stable native camera symbolic link.</param><param name="Width">The input width.</param>
/// <param name="Height">The input height.</param><param name="FpsNum">The advertised cadence numerator.</param>
/// <param name="FpsDen">The advertised cadence denominator.</param><param name="Subtype">The explicit native subtype.</param>
public sealed record RecordingCameraOptions(string DeviceId, int Width, int Height, int FpsNum, int FpsDen, string Subtype)
{
    /// <summary>Gets the explicit input label; actual observed cadence is measured by the source.</summary>
    public string Label => $"{Width} × {Height} · {(double)FpsNum / FpsDen:0.##} fps · {Subtype}";
}

/// <summary>Describes a native camera without retaining an opened device.</summary>
/// <param name="Id">The stable symbolic link.</param><param name="Name">The user-facing device name.</param>
/// <param name="Formats">The explicitly supported advertised input formats.</param>
public sealed record RecordingCameraDescriptor(string Id, string Name, IReadOnlyList<RecordingCameraOptions> Formats);

/// <summary>Defines the execution policy; Automatic resolves available stages before recording starts.</summary>
public enum RecordingExecutionMode
{
    /// <summary>Probes native hardware encoders, then x264; unavailable GPU capture may select FullSoftware before Start.</summary>
    Automatic,
    /// <summary>Requires the qualified capture and encoder on the same GPU.</summary>
    RequireGpuResidentHardware,
    /// <summary>Requires CPU capture, processing and encoding without opening a graphics device.</summary>
    FullSoftware,
    /// <summary>Requires GPU capture/processing and an explicit, measured readback for x264 CPU encoding.</summary>
    GpuWithSoftwareEncoding
}

/// <summary>Defines an explicit output size and bitrate within a qualified route.</summary>
/// <param name="Width">The even output width.</param><param name="Height">The even output height.</param>
/// <param name="BitrateBps">The target H264 bitrate.</param>
public sealed record RecordingVideoOptions(int Width, int Height, int BitrateBps);

/// <summary>Records the approved configuration and actual stage selection without exposing native handles.</summary>
/// <param name="RequestedMode">The requested policy.</param><param name="EffectiveMode">The selected policy.</param>
/// <param name="Video">The approved video preset, or null for audio-only sessions.</param><param name="StagesJson">The copied stage diagnostic JSON.</param>
/// <param name="Reason">The selection reason, including any preparation failure.</param>
public sealed record RecordingSelection(RecordingExecutionMode RequestedMode, RecordingExecutionMode EffectiveMode,
    RecordingVideoOptions? Video, string StagesJson, string Reason);

/// <summary>Declares observable lifecycle states independently of a frontend framework.</summary>
public enum RecordingState
{
    /// <summary>Owns no prepared sources.</summary>
    Created,
    /// <summary>Opens required sources.</summary>
    Preparing,
    /// <summary>Has prepared required sources.</summary>
    Ready,
    /// <summary>Establishes the media start.</summary>
    Starting,
    /// <summary>Records active content.</summary>
    Recording,
    /// <summary>Establishes the pause cut.</summary>
    Pausing,
    /// <summary>Excludes paused content.</summary>
    Paused,
    /// <summary>Resumes the existing timeline.</summary>
    Resuming,
    /// <summary>Stops admission and drains sources.</summary>
    Stopping,
    /// <summary>Closes assets and persists metadata.</summary>
    Finalizing,
    /// <summary>Has successfully finalized the recording.</summary>
    Completed,
    /// <summary>Preserves an operation failure.</summary>
    Failed,
    /// <summary>Has released the service.</summary>
    Disposed
}

/// <summary>Describes implemented features without claiming device availability.</summary>
/// <param name="Backend">The concrete managed backend.</param>
/// <param name="SupportsFullSoftware">A value indicating whole-chain software support.</param>
/// <param name="SupportsGpuResidentRecording">A value indicating qualified recording without pixel readback.</param>
/// <param name="SupportedFrameRates">The implemented output frame rates.</param>
public sealed record RecordingCapabilities(string Backend, bool SupportsFullSoftware,
    bool SupportsGpuResidentRecording, IReadOnlyList<int> SupportedFrameRates);

/// <summary>Describes available measurements; null means unavailable.</summary>
/// <param name="ActiveDuration">The useful duration from the finalized manifest.</param>
/// <param name="SubmittedVideoFrames">The submitted screen frame count, distinct from encoded packets.</param>
/// <param name="FinalizationDuration">The measured finalization operation time.</param>
public sealed record RecordingStatistics(TimeSpan? ActiveDuration, long? SubmittedVideoFrames,
    TimeSpan? FinalizationDuration);

/// <summary>Describes a successfully finalized session and measured metadata.</summary>
/// <param name="OutputDirectory">The finalized directory.</param>
/// <param name="PrimaryAssetPath">The finalized primary media file returned by the legacy control contract.</param>
/// <param name="ManifestPath">The authoritative manifest.</param>
/// <param name="Backend">The selected backend.</param>
/// <param name="Statistics">The available measurements.</param>
/// <param name="Selection">The approved native route and actual stage selections, when reported.</param>
/// <param name="CameraAssetPath">The effective independently finalized camera path, when selected.</param>
public sealed record RecordingResult(string OutputDirectory, string PrimaryAssetPath, string ManifestPath, string Backend,
    RecordingStatistics Statistics, RecordingSelection? Selection = null, string? CameraAssetPath = null);

/// <summary>Describes a state notification produced on the operation thread.</summary>
/// <param name="State">The resulting state.</param>
/// <param name="Error">The causal operation error, when present.</param>
public sealed record RecordingStateChangedEventArgs(RecordingState State, Exception? Error = null);

/// <summary>Creates owned services from UI-free requests.</summary>
public interface IRecorderEngineFactory
{
    /// <summary>Gets implemented backend features, independently of a device probe.</summary>
    RecordingCapabilities Capabilities { get; }
    /// <summary>Creates an owned service without starting capture.</summary>
    /// <param name="request">The requested sources and settings.</param>
    /// <returns>The service that the caller must dispose.</returns>
    IRecordingService Create(RecordingRequest request);
}

/// <summary>Controls and observes recording without Dispatcher, media buffers or native pointers.</summary>
public interface IRecordingService : IRecorderEngine, IAsyncDisposable
{
    /// <summary>Atomically updates an active scene and awaits its application on the native worker.</summary>
    Task UpdateSceneAsync(RecordingScene scene, CancellationToken cancellationToken = default) =>
        Task.FromException(new NotSupportedException("This recording backend does not support live scene updates."));
    /// <summary>Gets the approved route after preparation; null for services that do not report stage details.</summary>
    RecordingSelection? Selection => null;
    /// <summary>Occurs on the operation thread; consumers marshal to their own frontend thread.</summary>
    event EventHandler<RecordingStateChangedEventArgs>? StateChanged;
    /// <summary>Gets the latest observed state.</summary>
    RecordingState State { get; }
    /// <summary>Gets implemented backend features.</summary>
    RecordingCapabilities Capabilities { get; }
    /// <summary>Gets available measurements without fabricated zero counters.</summary>
    RecordingStatistics Statistics { get; }
    /// <summary>Gets the successful result, or null until real finalization succeeds.</summary>
    RecordingResult? Result { get; }
}

namespace NessStudio.Models;

/// <summary>Describes selected sources using values instead of UI or device handles.</summary>
public sealed class RecordingTargets
{
    /// <summary>Initializes the sources, preserving legacy camera and endpoint identities.</summary>
    /// <param name="screen">The optional monitor.</param>
    /// <param name="webcamName">The optional camera identity.</param>
    /// <param name="micDeviceId">The optional capture endpoint.</param>
    /// <param name="loopbackDeviceId">The optional render endpoint.</param>
    public RecordingTargets(MonitorDescriptor? screen, string? webcamName, string? micDeviceId, string? loopbackDeviceId)
    {
        Screen = screen;
        WebcamName = string.IsNullOrWhiteSpace(webcamName) ? null : webcamName;
        MicDeviceId = string.IsNullOrWhiteSpace(micDeviceId) ? null : micDeviceId;
        LoopbackDeviceId = string.IsNullOrWhiteSpace(loopbackDeviceId) ? null : loopbackDeviceId;
    }
    /// <summary>Gets the optional monitor snapshot.</summary>
    public MonitorDescriptor? Screen { get; }
    /// <summary>Gets the optional camera identity.</summary>
    public string? WebcamName { get; }
    /// <summary>Gets the optional capture endpoint.</summary>
    public string? MicDeviceId { get; }
    /// <summary>Gets the optional loopback endpoint.</summary>
    public string? LoopbackDeviceId { get; }
    /// <summary>Gets a value indicating whether screen capture is selected.</summary>
    public bool CaptureScreen => Screen is not null;
    /// <summary>Gets a value indicating whether camera capture is selected.</summary>
    public bool CaptureWebcam => WebcamName is not null;
    /// <summary>Gets a value indicating whether microphone capture is selected.</summary>
    public bool CaptureMic => MicDeviceId is not null;
    /// <summary>Gets a value indicating whether loopback capture is selected.</summary>
    public bool CaptureSystem => LoopbackDeviceId is not null;
    /// <summary>Gets a value indicating whether any video source is selected.</summary>
    public bool AnyVideo => CaptureScreen || CaptureWebcam;
    /// <summary>Gets a value indicating whether any audio source is selected.</summary>
    public bool AnyAudio => CaptureMic || CaptureSystem;
    /// <summary>Checks whether at least one source is selected.</summary>
    /// <param name="message">When this method returns, contains the error or null.</param>
    /// <returns>True if a source is selected.</returns>
    public bool IsValid(out string? message)
    { message = AnyVideo || AnyAudio ? null : "No source selected for recording."; return message is null; }
}

namespace NessStudio.Models;

/// <summary>Describes selected devices independently of enumeration APIs.</summary>
public sealed class RecordingDeviceSelection
{
    /// <summary>Initializes normalized device identities and the display label.</summary>
    /// <param name="webcamName">The optional camera.</param>
    /// <param name="micDeviceId">The optional capture endpoint.</param>
    /// <param name="loopbackDeviceId">The optional loopback endpoint.</param>
    /// <param name="displayFriendlyName">The optional display label.</param>
    public RecordingDeviceSelection(string? webcamName, string? micDeviceId, string? loopbackDeviceId, string? displayFriendlyName)
    { WebcamName = Normalize(webcamName); MicDeviceId = Normalize(micDeviceId); LoopbackDeviceId = Normalize(loopbackDeviceId); DisplayFriendlyName = displayFriendlyName?.Trim(); }
    /// <summary>Gets the optional camera.</summary>
    public string? WebcamName { get; }
    /// <summary>Gets the optional capture endpoint.</summary>
    public string? MicDeviceId { get; }
    /// <summary>Gets the optional loopback endpoint.</summary>
    public string? LoopbackDeviceId { get; }
    /// <summary>Gets the optional display label.</summary>
    public string? DisplayFriendlyName { get; }
    /// <summary>Gets a value indicating whether camera capture is selected.</summary>
    public bool CaptureWebcam => WebcamName is not null;
    /// <summary>Gets a value indicating whether microphone capture is selected.</summary>
    public bool CaptureMic => MicDeviceId is not null;
    /// <summary>Gets a value indicating whether loopback capture is selected.</summary>
    public bool CaptureSystem => LoopbackDeviceId is not null;
    /// <summary>Gets a value indicating whether a camera is selected.</summary>
    public bool AnyVideo => CaptureWebcam;
    /// <summary>Gets a value indicating whether an audio device is selected.</summary>
    public bool AnyAudio => CaptureMic || CaptureSystem;
    /// <summary>Creates the complete selection from devices and a monitor snapshot.</summary>
    /// <param name="screen">The optional monitor.</param>
    /// <returns>The complete recording source selection.</returns>
    public RecordingTargets BuildTargets(MonitorDescriptor? screen) => new(screen, WebcamName, MicDeviceId, LoopbackDeviceId);
    /// <summary>Checks whether a camera or audio endpoint is selected.</summary>
    /// <param name="message">When this method returns, contains the error or null.</param>
    /// <returns>True if a device is selected.</returns>
    public bool IsValid(out string? message) { message = AnyVideo || AnyAudio ? null : "No capture source was selected."; return message is null; }
    private static string? Normalize(string? value) => string.IsNullOrWhiteSpace(value) ? null : value.Trim();
}

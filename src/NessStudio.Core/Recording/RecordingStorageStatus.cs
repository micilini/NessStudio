using System.Globalization;

namespace NessStudio.Recording.Engines;

/// <summary>Reports usable output-volume space independently of recording state.</summary>
/// <param name="AvailableBytes">Bytes available to the current user, including volume quotas.</param>
public sealed record RecordingStorageStatus(ulong AvailableBytes)
{
    /// <summary>Gets the same 50 MiB start/stop boundary used by OBS.</summary>
    public const ulong StopBelowBytes = 50UL * 1024 * 1024;
    /// <summary>Gets the early-warning boundary; it does not stop recording.</summary>
    public const ulong WarnBelowBytes = 1024UL * 1024 * 1024;
    /// <summary>Gets whether capture should stop before the volume fills.</summary>
    public bool MustStop => AvailableBytes < StopBelowBytes;
    /// <summary>Gets whether the frontend should highlight limited free space.</summary>
    public bool IsLow => AvailableBytes < WarnBelowBytes;
    /// <summary>Gets a readable, English, binary-unit space value.</summary>
    public string FreeSpace => AvailableBytes >= WarnBelowBytes
        ? (Math.Floor(AvailableBytes / (double)WarnBelowBytes * 10) / 10).ToString("0.0", CultureInfo.InvariantCulture) + " GiB"
        : (Math.Floor(AvailableBytes / (1024d * 1024) * 10) / 10).ToString("0.0", CultureInfo.InvariantCulture) + " MiB";
    /// <summary>Gets the recording-window status without implying a predicted recording duration.</summary>
    public string Message => IsLow
        ? $"Only {FreeSpace} of free disk space remain. Recording stops below 50 MiB."
        : $"Free disk space: {FreeSpace}";
}

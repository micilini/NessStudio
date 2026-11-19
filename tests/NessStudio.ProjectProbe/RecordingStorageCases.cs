using System.Globalization;
using NessStudio.Diagnostics;
using NessStudio.Interop.Native;
using NessStudio.Recording.Engines;

internal static partial class Program
{
    private static void RecordingStorageCases()
    {
        Case("recording-storage-exact-50mib-boundary", () =>
        {
            Check(new RecordingStorageStatus(RecordingStorageStatus.StopBelowBytes - 1).MustStop,
                "Recording would continue below 50 MiB.");
            Check(!new RecordingStorageStatus(RecordingStorageStatus.StopBelowBytes).MustStop &&
                  !new RecordingStorageStatus(RecordingStorageStatus.StopBelowBytes + 1).MustStop,
                "The 50 MiB limit stops recording too early.");
        });
        Case("recording-storage-warning-is-not-a-stop", () =>
        {
            var status = new RecordingStorageStatus(500UL * 1024 * 1024);
            Check(status.IsLow && !status.MustStop && status.Message.Contains("500.0 MiB") &&
                  status.Message.Contains("50 MiB"), "Early warning lost the free-space value or actual limit.");
        });
        Case("recording-storage-english-binary-unit-format", () =>
        {
            var previous = CultureInfo.CurrentCulture;
            try
            {
                CultureInfo.CurrentCulture = CultureInfo.GetCultureInfo("pt-BR");
                Check(new RecordingStorageStatus(3UL * 1024 * 1024 * 1024 / 2).FreeSpace == "1.5 GiB" &&
                      new RecordingStorageStatus(0).FreeSpace == "0.0 MiB", "Space formatting changed with the Windows culture.");
            }
            finally { CultureInfo.CurrentCulture = previous; }
        });
        Case("recording-storage-start-preflight-honors-boundary", () =>
        {
            Reject<InvalidOperationException>(() => RecordingStorageSpace.EnsureCanStart(_root, _ => RecordingStorageStatus.StopBelowBytes - 1));
            Check(!RecordingStorageSpace.EnsureCanStart(_root, _ => RecordingStorageStatus.StopBelowBytes).MustStop,
                "Start was rejected at exactly 50 MiB.");
        });
        Case("recording-storage-native-disk-full-is-actionable", () =>
        {
            foreach (int code in new[] { 39, 112 })
            {
                var error = new NativeEngineException(-11, 7, unchecked((int)(0x80070000u | (uint)code)), "WriteFile");
                Check(EnglishErrors.Message(error).Contains("free disk space"), "Native disk-full failure remained an opaque WriteFile error.");
            }
        });
        Case("recording-storage-real-output-directory-query", () =>
        {
            ulong current = RecordingStorageSpace.AvailableBytes(_root);
            ulong child = RecordingStorageSpace.AvailableBytes(Path.Combine(_root, "not-created", "output"));
            Check(current > 0 && child > 0, "Space lookup did not resolve the output volume.");
        });
    }
}

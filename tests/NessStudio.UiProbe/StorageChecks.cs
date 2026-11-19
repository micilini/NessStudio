using System.IO;
using System.Text.Json;
using System.Windows;
using System.Windows.Controls;
using NessStudio;
using NessStudio.Recording;
using NessStudio.Recording.Engines;
using NessStudio.ViewModel;
using NessStudio.ViewModel.Helpers;

internal static partial class Program
{
    private static async Task RecordingStorageSafeguardsAsync(App app)
    {
        long available = 49L * 1024 * 1024;
        ulong Query(string _) => Volatile.Read(ref available) < 0
            ? throw new IOException("Controlled storage-query failure")
            : (ulong)Volatile.Read(ref available);
        var factory = new NativeRecorderEngineFactory(AppContext.BaseDirectory, DebugLog.Logger, Query);
        int notices = 0;
        var dialogs = new System.Windows.Threading.DispatcherTimer { Interval = TimeSpan.FromMilliseconds(150) };
        dialogs.Tick += (_, _) =>
        {
            foreach (var dialog in app.Windows.OfType<Window>().Where(window =>
                Equals(window.Tag, "EnglishMessageBox") && window.IsVisible).ToArray())
            {
                string text = string.Join(" ", Descendants<TextBlock>(dialog).Select(block => block.Text));
                if (text.Contains("stopped automatically"))
                {
                    Check(text.Contains("50 MiB") && text.Contains("Saved recording:"),
                        "Low-space stop did not explain the limit and preserved source files.");
                    notices++;
                    Screenshot(dialog, "storage-stop-notice-" + notices);
                }
                dialog.Close();
            }
        };
        dialogs.Start();
        try
        {
            var rejected = await NewRecorderAsync(app, RecordingExecutionMode.Automatic, 1, factory, Query);
            var rejectedVm = (RecordingScreenWindowVM)rejected.DataContext;
            await UntilAsync(() => rejectedVm.DiskSpaceMessage.Contains("49.0 MiB"), "Start preflight status did not show available space.");
            rejectedVm.HandleButtonAction(NessStudio.Models.MenuAction.StartRecording);
            await Operation(rejectedVm, "_startTask").WaitAsync(TimeSpan.FromSeconds(35));
            Check(rejectedVm.RecordingError?.Contains("50 MiB") == true && !rejectedVm.CanPauseRecording,
                "Recording began below 50 MiB without an actionable warning.");
            Check(!Directory.EnumerateFiles(Path.Combine(_output, "profile/Recordings"), "*.mkv", SearchOption.AllDirectories).Any(),
                "Rejected low-space start created media assets.");
            rejected.Close();
            await rejected.PreviewWorkspace.DisposeAsync();
            Results.Add(new { name = "storage-start-rejected-below-50mib-before-media-open" });

            var cases = Enum.GetValues<RecordingExecutionMode>().Select(mode => (mode, sources: 15))
                .Concat(new[] { (RecordingExecutionMode.FullSoftware, 4), (RecordingExecutionMode.FullSoftware, 8),
                                (RecordingExecutionMode.FullSoftware, 12) });
            foreach (var (mode, sources) in cases)
            {
                Interlocked.Exchange(ref available, 2L * 1024 * 1024 * 1024);
                var window = await NewRecorderAsync(app, mode, sources, factory, Query);
                var vm = (RecordingScreenWindowVM)window.DataContext;
                await UntilAsync(() => vm.DiskSpaceMessage.Contains("2.0 GiB"), "Normal free-space counter was not displayed.");
                await StartAsync(vm);
                await Task.Delay(3200);
                Interlocked.Exchange(ref available, 500L * 1024 * 1024);
                await UntilAsync(() => vm.IsDiskSpaceLow && vm.DiskSpaceMessage.Contains("500.0 MiB"), "Early low-space warning was not shown.");
                Check(vm.CanPauseRecording && Field(vm, "_stopTask") is null, "Early warning stopped the recording prematurely.");
                Screenshot(window, $"storage-warning-{mode}-{sources}");
                // An unavailable query must never be interpreted as zero bytes.
                Interlocked.Exchange(ref available, -1);
                await Task.Delay(1300);
                Check(vm.CanPauseRecording && vm.RecordingError is null, "A failed free-space query was treated as a full disk.");
                Interlocked.Exchange(ref available, (long)RecordingStorageStatus.StopBelowBytes);
                await Task.Delay(1300);
                Check(vm.CanPauseRecording && Field(vm, "_stopTask") is null, "Recording stopped at exactly 50 MiB.");
                int before = notices;
                Interlocked.Exchange(ref available, (long)RecordingStorageStatus.StopBelowBytes - 1);
                await UntilAsync(() => Field(vm, "_stopTask") is Task, "Recording did not stop automatically below 50 MiB.");
                await Operation(vm, "_stopTask").WaitAsync(TimeSpan.FromSeconds(150));
                Check(vm.RecordingError is null && notices == before + 1, "Low-space stop failed or displayed duplicate notices.");
                var project = ProjectManifestStore.Read(vm.OutDirFolder);
                Check(project.Manifest.State == "Completed" && project.MissingAssets.Count == 0,
                    "Low-space stop did not preserve a valid registered project.");
                Check(!File.Exists(Path.Combine(vm.OutDirFolder, "final.mkv")) && !File.Exists(Path.Combine(vm.OutDirFolder, "webcam.mp4")),
                    "Low-space stop started unnecessary derived-media copies.");
                using var native = NessStudio.Recording.ProjectMetadataStore.ReadDocument(vm.OutDirFolder, "recording-native.json")!;
                Check(native.RootElement.GetProperty("snapshot").GetProperty("mediaComplete").GetBoolean() &&
                      !native.RootElement.GetProperty("automaticExport").GetBoolean() &&
                      Directory.GetFiles(vm.OutDirFolder,"*.json").Select(Path.GetFileName).SequenceEqual(["manifest.json"]),
                    "Source-only saving and consolidated metadata were not recorded accurately.");
                foreach (var stream in native.RootElement.GetProperty("snapshot").GetProperty("metrics").GetProperty("audio").EnumerateArray())
                    Check(stream.GetProperty("lostSamples").GetInt64() == 0 && stream.GetProperty("deviceGapSamples").GetInt64() == 0,
                        "Low-space shutdown lost captured audio.");
                var files = new List<string>();
                if ((sources & 1) != 0) files.Add("screen.mkv");
                if ((sources & 2) != 0) files.Add("webcam.mkv");
                if ((sources & 4) != 0) files.Add("mic.wav");
                if ((sources & 8) != 0) files.Add("system.wav");
                foreach (string name in files) await DecodeExportAsync(Path.Combine(vm.OutDirFolder, name));
                Results.Add(new { name = "storage-automatic-50mib-stop-preserves-finalized-sources", mode = mode.ToString(),
                    sources, decoded = files, directory = vm.OutDirFolder });
                File.WriteAllText(Path.Combine(_output, "progress.json"), JsonSerializer.Serialize(Results));
                await window.PreviewWorkspace.DisposeAsync();
                foreach (Window open in app.Windows.Cast<Window>().ToArray()) open.Close();
            }
        }
        finally { dialogs.Stop(); }
    }
}

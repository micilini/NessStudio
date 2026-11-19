using System.Diagnostics;
using System.IO;
using System.Reflection;
using System.Text.Json;
using System.Windows;
using System.Windows.Controls;
using System.Windows.Media;
using NessStudio;
using NessStudio.Models;
using NessStudio.Recording;
using NessStudio.Recording.Engines;
using NessStudio.View.RecordingScreen;
using NessStudio.View.SavingRecordingScreen;
using NessStudio.ViewModel;

internal static partial class Program
{
    [System.Runtime.InteropServices.DllImport("user32.dll", CharSet = System.Runtime.InteropServices.CharSet.Unicode)]
    private static extern nint FindWindowEx(nint parent, nint after, string? className, string? title);
    [System.Runtime.InteropServices.DllImport("user32.dll")]
    private static extern uint GetWindowThreadProcessId(nint window, out uint processId);
    [System.Runtime.InteropServices.DllImport("user32.dll")]
    private static extern bool PostMessage(nint window, uint message, nint parameter, nint data);

    private static async Task PreserveFailedRecordingAsync()
    {
        // Dismiss only this probe's native error dialogs so the application's
        // real cleanup can preserve incomplete metadata before the probe exits.
        var timer = new System.Windows.Threading.DispatcherTimer { Interval = TimeSpan.FromMilliseconds(200) };
        timer.Tick += (_, _) =>
        {
            foreach (var message in Application.Current.Windows.OfType<Window>()
                .Where(window => Equals(window.Tag, "EnglishMessageBox")).ToArray()) message.Close();
            nint dialog = 0;
            while ((dialog = FindWindowEx(0, dialog, "#32770", null)) != 0)
            {
                GetWindowThreadProcessId(dialog, out uint owner);
                if (owner == Environment.ProcessId) PostMessage(dialog, 0x0111, 1, 0); // IDOK
            }
        };
        timer.Start();
        try
        {
            foreach (var window in Application.Current.Windows.OfType<RecordingScreenWindow>().ToArray())
                if (Field(window.DataContext, "_stopTask") is Task cleanup)
                    try { await cleanup.WaitAsync(TimeSpan.FromSeconds(120)); } catch { }
        }
        finally { timer.Stop(); }
    }

    private static void SavingLayout()
    {
        var saving = new SavingRecordingWindow { DataContext = new SavingRecordingWindowVM
            { Message = "Validating recorded video and audio tracks before saving the project." } };
        saving.Show();
        saving.UpdateLayout();
        var note = Descendants<TextBlock>(saving).Single(text => text.Text.StartsWith("Please keep"));
        Check(note.TextWrapping == TextWrapping.Wrap && note.ActualHeight > 22,
            "The saving instruction did not wrap onto multiple lines.");
        foreach (var text in Descendants<TextBlock>(saving))
        {
            var bounds = text.TransformToAncestor(saving).TransformBounds(new Rect(text.RenderSize));
            Check(new Rect(saving.RenderSize).Contains(bounds), "Saving window clipped its message: " + text.Text);
        }
        Screenshot(saving, "saving-wrapped-instruction");
        saving.AllowClose = true;
        saving.Close();
        Results.Add(new { name = "saving-window-instruction-wraps-and-all-text-fits" });
    }

    private static async Task<RecordingScreenWindow> NewRecorderAsync(App app, RecordingExecutionMode mode,
        int sources, IRecorderEngineFactory? factory = null, Func<string, ulong>? availableSpace = null)
    {
        var window = new RecordingScreenWindow(factory, availableSpace);
        window.Show();
        var vm = (RecordingScreenWindowVM)window.DataContext;
        await UntilAsync(() => vm.CanStartRecording, "Recorder did not load.");
        vm.SelectedExecutionMode = mode;
        vm.SelectedRecordingTimerSeconds = 0;
        vm.SelectedWebcam = vm.Webcams.First(camera => camera.Name.Contains("C920", StringComparison.OrdinalIgnoreCase));
        vm.SelectedMicrophone = vm.Microphones.First(device => device.FriendlyName.Contains("HyperX", StringComparison.OrdinalIgnoreCase));
        vm.IsScreenEnabled = (sources & 1) != 0;
        vm.IsWebcamEnabled = (sources & 2) != 0;
        vm.IsMicrophoneEnabled = (sources & 4) != 0;
        vm.IsSystemAudioEnabled = (sources & 8) != 0;
        return window;
    }

    private static async Task StartAsync(RecordingScreenWindowVM vm)
    {
        vm.HandleButtonAction(MenuAction.StartRecording);
        await Operation(vm, "_startTask").WaitAsync(TimeSpan.FromSeconds(120));
        Check(vm.CanPauseRecording && vm.RecordingError is null, "Recording did not start: " + vm.RecordingError);
    }

    private static async Task FinishAsync(App app, RecordingScreenWindow window, int sources, bool verify = true,
        int finalizationSeconds = 300)
    {
        var vm = (RecordingScreenWindowVM)window.DataContext;
        var stop = Stopwatch.StartNew();
        vm.HandleButtonAction(MenuAction.StopRecording);
        await Operation(vm, "_stopTask").WaitAsync(TimeSpan.FromSeconds(finalizationSeconds));
        long saveMilliseconds = stop.ElapsedMilliseconds;
        Check(vm.RecordingError is null && File.Exists(Path.Combine(vm.OutDirFolder, "manifest.json")),
            "Recording did not finalize: " + vm.RecordingError);
        if (verify) await VerifyMediaAsync(vm.OutDirFolder, sources, minimumSeconds: 3);
        Results.Add(new { name = "recording-stop-completes-with-validated-media", stopMilliseconds = saveMilliseconds,
            saveAndVerificationMilliseconds = stop.ElapsedMilliseconds,
            mode = vm.SelectedExecutionMode.ToString(), sources, directory = vm.OutDirFolder });
        File.WriteAllText(Path.Combine(_output, "progress.json"), JsonSerializer.Serialize(Results));
        await window.PreviewWorkspace.DisposeAsync();
        foreach (Window open in app.Windows.Cast<Window>().ToArray()) open.Close();
    }

    private static async Task RecordingRegressionsAsync(App app)
    {
        SavingLayout();
        foreach (var mode in Enum.GetValues<RecordingExecutionMode>())
        {
            var window = await NewRecorderAsync(app, mode, 1);
            var vm = (RecordingScreenWindowVM)window.DataContext;
            vm.SelectedScreenOption = vm.ScreenOptions.First(option => option.Value?.Primary == false);
            string monitor = vm.SelectedScreenOption.Display;
            await StartAsync(vm);
            await Task.Delay(4000);
            await FinishAsync(app, window, 1);
            Results.Add(new { name = "secondary-full-monitor-recording", mode = mode.ToString(), monitor,
                directory = vm.OutDirFolder });
        }
        // Repeat the exact real-device combination that hit a slow storage flush.
        for (int cycle = 0; cycle < 8; cycle++)
        {
            var window = await NewRecorderAsync(app, RecordingExecutionMode.FullSoftware, 3);
            var vm = (RecordingScreenWindowVM)window.DataContext;
            await StartAsync(vm);
            await Task.Delay(4000);
            await FinishAsync(app, window, 3);
        }
        // Repeat the GPU + microphone route whose WAV write paused for 659ms.
        for (int cycle = 0; cycle < 8; cycle++)
        {
            var window = await NewRecorderAsync(app, RecordingExecutionMode.RequireGpuResidentHardware, 5);
            await StartAsync((RecordingScreenWindowVM)window.DataContext);
            await Task.Delay(4000);
            await FinishAsync(app, window, 5);
        }
        // Record actual regions on both physical monitors in every execution mode.
        foreach (var mode in Enum.GetValues<RecordingExecutionMode>())
        foreach (bool secondary in new[] { false, true })
        {
            var window = await NewRecorderAsync(app, mode, 1);
            var vm = (RecordingScreenWindowVM)window.DataContext;
            var monitor = vm.ScreenOptions.First(item => item.Value is not null && item.Value.Primary != secondary);
            var bounds = monitor.Value!.Bounds;
            var region = new Rect(bounds.X + 160, bounds.Y + 120, 640, 360);
            typeof(RecordingScreenWindowVM).GetMethod("ApplyDrawArea", BindingFlags.NonPublic | BindingFlags.Instance)!.Invoke(vm, [region]);
            await StartAsync(vm);
            await Task.Delay(2000);
            vm.HandleButtonAction(MenuAction.PauseRecording);
            await UntilAsync(() => vm.CurrentRecordingState == RecordingState.Paused, "Region recording did not pause.", () => vm.RecordingError);
            await Task.Delay(400);
            vm.HandleButtonAction(MenuAction.PauseRecording);
            await UntilAsync(() => vm.CurrentRecordingState == RecordingState.Recording, "Region recording did not resume.", () => vm.RecordingError);
            await Task.Delay(2000);
            await FinishAsync(app, window, 1);
            using var diagnostics = NessStudio.Recording.ProjectMetadataStore.ReadDocument(vm.OutDirFolder, "recording-native.json")!;
            Check(diagnostics.RootElement.GetRawText().Contains("global-physical"),
                "Region recording lost its physical coordinate space.");
            Results.Add(new { name = "region-recording-pause-resume-and-finalization", mode = mode.ToString(),
                secondary, crop = region.ToString(), directory = vm.OutDirFolder });
        }
    }

    private static async Task RecordingPauseAllSourcesAsync(App app)
    {
        foreach (var mode in Enum.GetValues<RecordingExecutionMode>())
        {
            var window = await NewRecorderAsync(app, mode, 15);
            var vm = (RecordingScreenWindowVM)window.DataContext;
            var microphoneLevel = (System.Windows.Controls.Slider)window.FindName("sldMicVolume");
            var systemLevel = (System.Windows.Controls.Slider)window.FindName("sldSystemVolume");
            microphoneLevel.Value = mode switch { RecordingExecutionMode.Automatic => 0,
                RecordingExecutionMode.RequireGpuResidentHardware => 25, RecordingExecutionMode.FullSoftware => 50, _ => 100 };
            systemLevel.Value = 100 - microphoneLevel.Value;
            Check(vm.MicrophoneLevel == microphoneLevel.Value && vm.SystemAudioLevel == systemLevel.Value,
                "Recording level sliders are not connected to the request properties.");
            await StartAsync(vm);
            Check(!microphoneLevel.IsEnabled && !systemLevel.IsEnabled,
                "Recording levels remained editable after the capture gain was fixed.");
            await Task.Delay(2200);
            vm.HandleButtonAction(MenuAction.PauseRecording);
            await UntilAsync(() => vm.CurrentRecordingState == RecordingState.Paused,
                "Four-source recording did not pause.", () => vm.RecordingError);
            await Task.Delay(600);
            vm.HandleButtonAction(MenuAction.PauseRecording);
            await UntilAsync(() => vm.CurrentRecordingState == RecordingState.Recording,
                "Four-source recording did not resume.", () => vm.RecordingError);
            await Task.Delay(2200);
            await FinishAsync(app, window, 15);
            using var document = NessStudio.Recording.ProjectMetadataStore.ReadDocument(vm.OutDirFolder, "recording-native.json")!;
            var snapshot = document.RootElement.GetProperty("snapshot");
            Check(snapshot.GetProperty("metrics").GetProperty("activeIntervals").GetArrayLength() == 2,
                "Four-source recording did not retain the two shared active intervals.");
            foreach (var stream in snapshot.GetProperty("metrics").GetProperty("audio").EnumerateArray())
            {
                double expectedGain = stream.GetProperty("kind").GetString() == "microphone"
                    ? vm.MicrophoneLevel / 100 : vm.SystemAudioLevel / 100;
                Check(Math.Abs(stream.GetProperty("gain").GetDouble() - expectedGain) < 0.000001,
                    "The native track did not apply the selected recording level.");
                Check(stream.GetProperty("lostSamples").GetInt64() == 0 &&
                    stream.GetProperty("deviceGapSamples").GetInt64() == 0,
                    "Pause/resume lost real microphone or loopback samples.");
                var storage = stream.GetProperty("storageWrites");
                Check(storage.GetProperty("pendingBytes").GetInt64() == 0 &&
                    storage.GetProperty("submittedBytes").GetInt64() == storage.GetProperty("completedBytes").GetInt64(),
                    "Pause/resume finalized before draining owned audio writes.");
            }
            Results.Add(new { name = "four-real-sources-pause-resume-common-clock", mode = mode.ToString(),
                directory = vm.OutDirFolder });
        }
    }

    private sealed class BoundedSoakFactory(IRecorderEngineFactory inner, bool preserveResolution = false) : IRecorderEngineFactory
    {
        public RecordingCapabilities Capabilities => inner.Capabilities;
        public IRecordingService Create(RecordingRequest request)
        {
            if (!request.Targets.AnyVideo) throw new InvalidOperationException("The endurance qualification requires video sources.");
            var bounds = request.Crop ?? request.Targets.Screen?.Bounds;
            var video = request.Video ?? new RecordingVideoOptions(bounds?.Width ?? request.Camera?.Width ?? 1280,
                bounds?.Height ?? request.Camera?.Height ?? 720, 1_000_000);
            return inner.Create(request with
                { QualityPreset = null, Video = new RecordingVideoOptions(preserveResolution ? video.Width : 640,
                    preserveResolution ? video.Height : 360, 1_000_000) });
        }
    }

    private static async Task RecordingCyclesAsync(App app, int count)
    {
        using var process = Process.GetCurrentProcess();
        var modes = Enum.GetValues<RecordingExecutionMode>();
        for (int cycle = 0; cycle < count; cycle++)
        {
            var mode = modes[cycle % modes.Length];
            var window = await NewRecorderAsync(app, mode, 15);
            await StartAsync((RecordingScreenWindowVM)window.DataContext);
            // Cross the former five-second clock-estimate failure boundary.
            await Task.Delay(6000);
            await FinishAsync(app, window, 15);
            await UntilAsync(() => NessStudio.Interop.Native.NativeEngine.WaitForQuiescence(0),
                "A start/stop cycle retained native capture/encoder resources.", seconds: 10);
            if ((cycle + 1) % 10 == 0) { GC.Collect(); GC.WaitForPendingFinalizers(); GC.Collect(); }
            process.Refresh();
            File.AppendAllText(Path.Combine(_output, "cycle-resources.jsonl"), JsonSerializer.Serialize(new
                { cycle = cycle + 1, mode = mode.ToString(), process.WorkingSet64, process.PrivateMemorySize64,
                  process.HandleCount, threads = process.Threads.Count, managedBytes = GC.GetTotalMemory(false) }) + Environment.NewLine);
            Results.Add(new { name = "full-sources-cycle-releases-native-resources", cycle = cycle + 1, mode = mode.ToString() });
            File.WriteAllText(Path.Combine(_output, "progress.json"), JsonSerializer.Serialize(Results));
        }
    }

    private static async Task SoakAsync(App app, int minutes)
    {
        SavingLayout();
        var window = await NewRecorderAsync(app, RecordingExecutionMode.RequireGpuResidentHardware, 15,
            _soakNativeResolution && !_soakLowBitrate ? null :
                new BoundedSoakFactory(app.Services.RecorderFactory, _soakNativeResolution));
        var vm = (RecordingScreenWindowVM)window.DataContext;
        await StartAsync(vm);
        Check(vm.EffectiveRecordingSelection?.StagesJson.Contains("NvencD3D11") == true,
            "The soak test did not select real NVIDIA NVENC.");
        using var process = Process.GetCurrentProcess();
        var clock = Stopwatch.StartNew();
        int samples = 0;
        while (clock.Elapsed < TimeSpan.FromMinutes(minutes))
        {
            await Task.Delay(10000);
            Check(vm.CanPauseRecording && vm.RecordingError is null, "Long recording failed: " + vm.RecordingError);
            process.Refresh();
            var sample = new { seconds = clock.Elapsed.TotalSeconds, pid = process.Id,
                workingSetBytes = process.WorkingSet64, privateBytes = process.PrivateMemorySize64,
                handles = process.HandleCount, threads = process.Threads.Count, managedBytes = GC.GetTotalMemory(false) };
            await File.AppendAllTextAsync(Path.Combine(_output, "soak-samples.jsonl"), JsonSerializer.Serialize(sample) + Environment.NewLine);
            samples++;
        }
        await FinishAsync(app, window, 15, finalizationSeconds: 300);
        Results.Add(new { name = "continuous-full-application-recording", minutes, samples,
            camera = "Logitech C920", microphone = "HyperX QuadCast", video = _soakLowBitrate
                ? "Native monitor/camera resolution, 30 fps, 1 Mbps per encoded track"
                : _soakNativeResolution ? "Native monitor/camera resolution with default encoder settings"
                : "640x360 30 fps 1 Mbps per track",
            encoder = "NVIDIA NVENC", directory = vm.OutDirFolder });
    }
}

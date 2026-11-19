using System.Diagnostics;
using System.IO;
using System.Reflection;
using System.Text.Json;
using System.Windows;
using System.Windows.Controls;
using System.Windows.Interop;
using System.Windows.Media;
using System.Windows.Media.Imaging;
using NessStudio;
using NessStudio.Composition;
using NessStudio.Components.Menu;
using NessStudio.Interop.Native;
using NessStudio.Models;
using NessStudio.Presentation;
using NessStudio.Recording;
using NessStudio.Recording.Engines;
using NessStudio.View.RecordingScreen;
using NessStudio.View.SplashScreen;
using NessStudio.ViewModel;
using NessStudio.ViewModel.Helpers;

internal static partial class Program
{
    private static readonly List<object> Results = [];
    private static string _output = "";
    private static RecordingExecutionMode? _observeMode;
    private static string? _workflowProject;
    private static bool _regressions;
    private static bool _dialogs;
    private static bool _pauseAllSources;
    private static bool _storageSafeguards;
    private static bool _recorderRefinements;
    private static bool _workstationPreviews;
    private static bool _gpuOnlyPreviews;
    private static int _soakMinutes;
    private static bool _soakNativeResolution;
    private static bool _soakLowBitrate;
    private static int _stressCycles;
    private static void Check(bool value, string message) { if (!value) throw new InvalidDataException(message); }

    [STAThread]
    private static int Main(string[] args)
    {
        if (args.Length is < 1 or > 3) return 2;
        string output = args[0];
        if (args.Length == 3 && args[1] == "workflow") _workflowProject = Path.GetFullPath(args[2]);
        else if (args.Length == 3 && args[1] is "soak" or "soak-native" or "soak-native-1mbps")
        {
            _soakMinutes = Math.Clamp(int.Parse(args[2]), 1, 120);
            _soakNativeResolution = args[1] != "soak";
            _soakLowBitrate = args[1] == "soak-native-1mbps";
        }
        else if (args.Length == 3 && args[1] == "cycles") _stressCycles = Math.Clamp(int.Parse(args[2]), 1, 200);
        else if (args.Length == 2 && args[1] == "regressions") _regressions = true;
        else if (args.Length == 2 && args[1] == "dialogs") _dialogs = true;
        else if (args.Length == 2 && args[1] == "pause-sources") _pauseAllSources = true;
        else if (args.Length == 2 && args[1] == "storage") _storageSafeguards = true;
        else if (args.Length == 2 && args[1] == "recorder-refinements") _recorderRefinements = true;
        else if (args.Length == 2 && args[1] == "previews") _workstationPreviews = true;
        else if (args.Length == 2 && args[1] == "previews-gpu") { _workstationPreviews = true; _gpuOnlyPreviews = true; }
        else if (args.Length == 2) _observeMode = Enum.Parse<RecordingExecutionMode>(args[1]);
        else if (args.Length == 3) return 2;
        _output = Path.GetFullPath(output);
        if (Directory.Exists(_output)) throw new IOException("Use a new evidence directory.");
        Directory.CreateDirectory(_output);
        var options = new ApplicationOptions(Path.Combine(_output, "profile"), Path.Combine(_output, "legacy-documents"),
            @"Local\NessStudio.UiProbe." + Guid.NewGuid().ToString("N"));
        var app = new App(options);
        app.InitializeComponent();
        // Use the application's real resources while the probe controls window creation.
        typeof(Application).GetField("_startupUri", BindingFlags.Instance | BindingFlags.NonPublic)!.SetValue(app, null);
        app.ShutdownMode = ShutdownMode.OnExplicitShutdown;
        app.Startup += async (_, _) =>
        {
            // Keep unattended failure runs inspectable without leaving a modal
            // on the user's desktop. The recording assertions still fail and
            // retain the error log, native diagnostics and screenshot.
            var errorDialogs = new System.Windows.Threading.DispatcherTimer { Interval = TimeSpan.FromMilliseconds(500) };
            errorDialogs.Tick += (_, _) =>
            {
                if (!app.Windows.OfType<RecordingScreenWindow>().Any(window =>
                    window.DataContext is RecordingScreenWindowVM { RecordingError: not null })) return;
                foreach (var dialog in app.Windows.OfType<Window>()
                    .Where(window => Equals(window.Tag, "EnglishMessageBox") && window.IsVisible).ToArray())
                {
                    try { Screenshot(dialog, "recording-error-dialog"); } catch { }
                    dialog.Close();
                }
            };
            errorDialogs.Start();
            try
            {
                await Task.Run(app.Services.LoadConfiguration);
                app.DebugLogsEnabled = false;
                DebugLog.Write("UI probe: this debug message must stay disabled.");
                Check(!File.Exists(DebugLog.GetPath()), "Disabled debug logging created a diagnostic file.");
                DebugLog.WriteError("UI probe: failure logging remains available without verbose diagnostics.");
                using (var stream = new FileStream(DebugLog.GetErrorPath(), FileMode.Open, FileAccess.Read, FileShare.ReadWrite))
                using (var reader = new StreamReader(stream))
                    Check(reader.ReadToEnd().Contains("failure logging remains available"), "Failure log was not persisted.");
                Check(!File.Exists(DebugLog.GetPath()), "Failure logging wrote to the disabled diagnostic file.");
                app.DebugLogsEnabled = true;
                Results.Add(new { name = "failure-log-without-verbose-diagnostics" });
                if (_workflowProject is not null) await WorkflowAsync(app, _workflowProject);
                else if (_dialogs) EnglishDialogs();
                else if (_regressions) await RecordingRegressionsAsync(app);
                else if (_pauseAllSources) await RecordingPauseAllSourcesAsync(app);
                else if (_storageSafeguards) await RecordingStorageSafeguardsAsync(app);
                else if (_recorderRefinements) await RecorderRefinementsAsync(app);
                else if (_workstationPreviews) await WorkstationAsync(app);
                else if (_stressCycles > 0) await RecordingCyclesAsync(app, _stressCycles);
                else if (_soakMinutes > 0) await SoakAsync(app, _soakMinutes);
                else
                {
                    GpuLockRetry();
                    await SplashAsync();
                    if (_observeMode is null) await WorkstationAsync(app);
                    await RecorderAsync(app);
                }
                await UntilAsync(() => NativeEngine.WaitForQuiescence(0), "Native preview/recording resources did not quiesce.", seconds: 5);
                File.WriteAllText(Path.Combine(_output, "result.json"), JsonSerializer.Serialize(new { status = "PASS", cases = Results }, new JsonSerializerOptions { WriteIndented = true }));
                app.Shutdown(0);
            }
            catch (Exception error)
            {
                CaptureDiagnostics();
                File.Copy(Path.Combine(_output, "diagnostics.json"), Path.Combine(_output, "diagnostics-before-cleanup.json"), true);
                await PreserveFailedRecordingAsync();
                CaptureDiagnostics();
                File.WriteAllText(Path.Combine(_output, "result.json"), JsonSerializer.Serialize(new { status = "FAIL", error = error.ToString(), cases = Results }, new JsonSerializerOptions { WriteIndented = true }));
                Console.Error.WriteLine(error);
                Environment.Exit(1);
            }
            finally { errorDialogs.Stop(); }
        };
        return app.Run();
    }

    private static object? Field(object target, string name) => target.GetType().GetField(name, BindingFlags.Instance | BindingFlags.NonPublic)?.GetValue(target);

    private static void CaptureDiagnostics()
    {
        var windows = new List<object>();
        foreach (var window in Application.Current.Windows.OfType<RecordingScreenWindow>())
        {
            var sources = new List<JsonElement>();
            var workspace = window.PreviewWorkspace;
            JsonElement? recordingSnapshot = null;
            lock (Field(workspace, "_gate")!)
            {
                if (Field(workspace, "_recording") is NativeSession recording)
                {
                    using var snapshot = recording.GetSnapshot();
                    recordingSnapshot = snapshot.RootElement.Clone();
                }
                foreach (System.Collections.DictionaryEntry entry in (System.Collections.IDictionary)Field(workspace, "_idleSources")!)
                {
                    var session = (NativeSession)entry.Value!.GetType().GetProperty("Session")!.GetValue(entry.Value)!;
                    using var snapshot = session.GetSnapshot();
                    sources.Add(snapshot.RootElement.Clone());
                }
            }
            JsonElement? finalSnapshot = null;
            if (Field(window.DataContext, "_rec") is { } service && Field(service, "_finalSnapshot") is string finalJson)
            {
                using var document = JsonDocument.Parse(finalJson);
                finalSnapshot = document.RootElement.Clone();
            }
            windows.Add(new
            {
                window.IsVisible, sources, recordingSnapshot, finalSnapshot,
                disposed = Field(workspace, "_disposed"), suspended = Field(workspace, "_suspended"), revision = Field(workspace, "_revision"),
                pendingControl = ((SemaphoreSlim)Field(workspace, "_control")!).CurrentCount == 0,
                opening = Field(workspace, "_openingSource") is not null,
                closing = Field(workspace, "_closingSource") is not null,
                previews = Descendants<NativePreviewControl>(window).Select(preview => new
                {
                    preview.SourceId, preview.IsVisible, preview.IsLoaded, preview.PresentedFrames, preview.PresentationError,
                    preview.PresentationKind, preview.GpuSurfaceBindings,
                    subscribed = Field(preview, "_subscribed"), rebind = Field(preview, "_rebind"),
                    attached = Field(preview, "_preview") is not null, pending = Field(preview, "_pendingGpuFrame") is not null,
                    gpuWriteLocked = Field(preview, "_gpuWriteLocked"),
                    imageLocks = Field(preview, "_image") is { } image ? Field(image, "_lockCount") : null
                }).ToArray()
            });
        }
        File.WriteAllText(Path.Combine(_output, "diagnostics.json"), JsonSerializer.Serialize(windows, new JsonSerializerOptions { WriteIndented = true }));
    }

    private static async Task UntilAsync(Func<bool> ready, string message, Func<string?>? error = null, int seconds = 120)
    {
        var clock = Stopwatch.StartNew();
        while (!ready())
        {
            if (error?.Invoke() is { } failure) throw new InvalidDataException(message + ": " + failure);
            if (clock.Elapsed.TotalSeconds > seconds) throw new TimeoutException(message);
            await Task.Delay(40);
        }
    }

    private static void GpuLockRetry()
    {
        var image = new D3DImage();
        var acquire = typeof(NativePreviewControl).GetMethod("TryLockImage", BindingFlags.NonPublic | BindingFlags.Static)!;
        var available = (ManualResetEvent)typeof(D3DImage).GetField("_canWriteEvent", BindingFlags.NonPublic | BindingFlags.Instance)!.GetValue(image)!;
        // Force the same timeout as a busy compositor without depending on GPU load.
        available.Reset();
        for (int attempt = 0; attempt < 100; attempt++)
            Check(!(bool)acquire.Invoke(null, [image])!, "A busy compositor was treated as writable.");
        available.Set();
        Check((bool)acquire.Invoke(null, [image])!, "A timed-out WPF lock prevented subsequent presentation.");
        image.Unlock();
        Results.Add(new { name = "gpu-presentation-recovers-after-100-lock-timeouts" });
    }

    private static IEnumerable<T> Descendants<T>(DependencyObject parent) where T : DependencyObject
    {
        for (int i = 0; i < VisualTreeHelper.GetChildrenCount(parent); i++)
        {
            var child = VisualTreeHelper.GetChild(parent, i);
            if (child is T value) yield return value;
            foreach (var nested in Descendants<T>(child)) yield return nested;
        }
    }

    private static void Screenshot(FrameworkElement view, string name)
    {
        var bitmap = new RenderTargetBitmap((int)Math.Ceiling(view.ActualWidth), (int)Math.Ceiling(view.ActualHeight), 96, 96, PixelFormats.Pbgra32);
        bitmap.Render(view);
        var encoder = new PngBitmapEncoder();
        encoder.Frames.Add(BitmapFrame.Create(bitmap));
        using var file = File.Create(Path.Combine(_output, name + ".png"));
        encoder.Save(file);
    }

    private static async Task SplashAsync()
    {
        var splash = new SplashScreenWindow();
        var callback = typeof(SplashScreenWindow).GetMethod("Window_ContentRendered", BindingFlags.NonPublic | BindingFlags.Instance)!;
        splash.ContentRendered -= (EventHandler)callback.CreateDelegate(typeof(EventHandler), splash);
        splash.Show();
        await Task.Delay(250);
        var progress = Descendants<ProgressBar>(splash).Single();
        var indicator = (Border)progress.Template.FindName("SolidIndicator", progress);
        var sweep = (GradientStop)progress.Template.FindName("LoadingCenter", progress);
        double initialOffset = sweep.Offset;
        Check(indicator.ActualWidth > 100 && Math.Abs(indicator.ActualWidth - progress.ActualWidth) < 1, "Splash indicator is not continuous.");
        for (int sample = 0; sample < 4; sample++)
        {
            var bitmap = new RenderTargetBitmap((int)indicator.ActualWidth, (int)indicator.ActualHeight, 96, 96, PixelFormats.Pbgra32);
            bitmap.Render(indicator);
            int stride = bitmap.PixelWidth * 4;
            byte[] pixels = new byte[stride * bitmap.PixelHeight];
            bitmap.CopyPixels(pixels, stride, 0);
            int middle = bitmap.PixelHeight / 2 * stride;
            for (int x = 6; x < bitmap.PixelWidth - 6; x++)
                for (int channel = 0; channel < 4; channel++)
                    Check(pixels[middle + x * 4 + channel] == pixels[middle + 6 * 4 + channel], "Splash fill contains gaps or fragments.");
            await Task.Delay(250);
        }
        Screenshot(splash, "splash");
        Check(Math.Abs(sweep.Offset - initialOffset) > 0.1, "Splash loading bar did not travel across the blue track.");
        splash.Close();
        Results.Add(new { name = "continuous-splash-fill-with-moving-loading-bar", samples = 4 });
    }

    private static async Task RecorderAsync(App app)
    {
        var window = new RecordingScreenWindow();
        window.Show();
        var vm = (RecordingScreenWindowVM)window.DataContext;
        await UntilAsync(() => vm.CanStartRecording, "Recorder did not load.");
        Check(Math.Abs(window.Width - 740) < 1 && Math.Abs(window.Height - 316) < 1,
            $"Recorder dimensions do not include the audio meters and footer: {window.Width} x {window.Height}.");
        var diskStatus = (TextBlock)window.FindName("DiskSpaceStatusText");
        Check(diskStatus.IsVisible && diskStatus.ActualHeight > 0 &&
              new Rect(window.RenderSize).Contains(diskStatus.TransformToAncestor(window).TransformBounds(new Rect(diskStatus.RenderSize))),
            "The free-disk-space status is missing or clipped.");
        Check(Descendants<NativePreviewControl>(window).Count() == 2, "Unexpected program preview in recorder.");
        var switches = Descendants<System.Windows.Controls.Primitives.ToggleButton>(window)
            .Where(toggle => toggle.GetBindingExpression(System.Windows.Controls.Primitives.ToggleButton.IsCheckedProperty)?.ParentBinding.Path?.Path is "IsScreenEnabled" or "IsWebcamEnabled").ToArray();
        Check(switches.Length == 2, "Source switches not found.");
        Check(Math.Abs(switches[0].TranslatePoint(new Point(), window).Y - switches[1].TranslatePoint(new Point(), window).Y) < 1, "Camera switch is misaligned.");
        Check(vm.Webcams.Count > 0, "Connect a camera to run this hardware UI probe.");
        vm.SelectedWebcam = vm.Webcams.FirstOrDefault(camera => camera.Name.Contains("c920", StringComparison.OrdinalIgnoreCase))
            ?? vm.Webcams.FirstOrDefault(camera => camera.Name.Contains("integrat", StringComparison.OrdinalIgnoreCase))
            ?? vm.Webcams.FirstOrDefault(camera => !camera.Name.Contains("c920", StringComparison.OrdinalIgnoreCase)
                && !camera.Name.Contains("virtual", StringComparison.OrdinalIgnoreCase)) ?? vm.Webcams[0];
        var screen = (NativePreviewControl)window.FindName("ScreenNativePreview");
        var camera = (NativePreviewControl)window.FindName("CameraNativePreview");
        // Source startup must survive collections even though UI refresh requests
        // are asynchronous and the caller does not await each selection change.
        Task startupCollections = Task.Run(async () =>
        {
            for (int attempt = 0; attempt < 16; attempt++)
            {
                await Task.Delay(50);
                GC.Collect();
                GC.WaitForPendingFinalizers();
            }
        });
        if (_observeMode is { } observed)
        {
            vm.SelectedExecutionMode = observed;
            vm.IsScreenEnabled = true;
            vm.IsWebcamEnabled = true;
            await UntilAsync(() => screen.PresentedFrames > 3 && camera.PresentedFrames > 3,
                "Observation previews did not start.", () => screen.PresentationError ?? camera.PresentationError ?? window.PreviewWorkspace.Error);
            await startupCollections;
            Results.Add(new { name = "source-startup-survives-forced-gc", collections = 16 });
            File.WriteAllText(Path.Combine(_output, "ready.json"), JsonSerializer.Serialize(new { mode = observed.ToString(), window.Left, window.Top, window.Width, window.Height }));
            var observation = Stopwatch.StartNew();
            long previousScreen = screen.PresentedFrames, previousCamera = camera.PresentedFrames;
            var heartbeat = Stopwatch.StartNew();
            while (window.IsVisible && observation.Elapsed < TimeSpan.FromMinutes(8)
                && !File.Exists(Path.Combine(_output, "finish-observation")))
            {
                await Task.Delay(250);
                if (heartbeat.Elapsed < TimeSpan.FromSeconds(3)) continue;
                Check(screen.PresentedFrames > previousScreen && camera.PresentedFrames > previousCamera,
                    $"Observation preview stalled at {observation.Elapsed}: screen {screen.PresentedFrames}, camera {camera.PresentedFrames}; {screen.PresentationError} {camera.PresentationError}");
                previousScreen = screen.PresentedFrames;
                previousCamera = camera.PresentedFrames;
                File.AppendAllText(Path.Combine(_output, "heartbeat.jsonl"), JsonSerializer.Serialize(new
                {
                    elapsedSeconds = observation.Elapsed.TotalSeconds,
                    screenFrames = previousScreen, cameraFrames = previousCamera,
                    screenBindings = screen.GpuSurfaceBindings, cameraBindings = camera.GpuSurfaceBindings,
                    workingSet = Environment.WorkingSet
                }) + Environment.NewLine);
                heartbeat.Restart();
            }
            Screenshot(window, "observation");
            Results.Add(new { name = "visual-observation", mode = observed.ToString(), screenFrames = screen.PresentedFrames, cameraFrames = camera.PresentedFrames });
            await window.PreviewWorkspace.DisposeAsync();
            window.Close();
            return;
        }
        foreach (var mode in Enum.GetValues<RecordingExecutionMode>())
        {
            vm.IsScreenEnabled = false;
            vm.IsWebcamEnabled = false;
            await window.PreviewWorkspace.UpdateIdleAsync(null, null, mode);
            vm.SelectedExecutionMode = mode;
            vm.IsScreenEnabled = true;
            long first = screen.PresentedFrames;
            await UntilAsync(() => screen.Source is not null && screen.PresentedFrames > first + 3, "Screen preview did not start: " + mode, () => screen.PresentationError);
            ulong screenGeneration = screen.PresentedGeneration;
            int screenBlanks = 0;
            EventHandler observeScreen = (_, _) => { if (screen.Source is null) screenBlanks++; };
            CompositionTarget.Rendering += observeScreen;
            try
            {
                vm.IsWebcamEnabled = true;
                long cameraStart = camera.PresentedFrames;
                await UntilAsync(() => camera.Source is not null && camera.PresentedFrames > cameraStart + 3, "Camera preview did not start: " + mode, () => camera.PresentationError);
                Check(screen.PresentedGeneration == screenGeneration, "Enabling camera recreated screen preview.");
                vm.IsWebcamEnabled = false;
                GC.Collect();
                GC.WaitForPendingFinalizers();
                await Task.Delay(400);
                vm.IsWebcamEnabled = true;
                cameraStart = camera.PresentedFrames;
                await UntilAsync(() => camera.Source is not null && camera.PresentedFrames > cameraStart + 3, "Camera preview did not restart: " + mode, () => camera.PresentationError);
                Check(screen.PresentedGeneration == screenGeneration && screenBlanks == 0, "Camera toggle interrupted the monitor.");
                if (mode == RecordingExecutionMode.Automatic)
                {
                    await startupCollections;
                    Results.Add(new { name = "source-startup-survives-forced-gc", collections = 16 });
                }
            }
            finally { CompositionTarget.Rendering -= observeScreen; }
            ulong cameraGeneration = camera.PresentedGeneration;
            vm.IsScreenEnabled = false;
            await Task.Delay(400);
            vm.IsScreenEnabled = true;
            first = screen.PresentedFrames;
            await UntilAsync(() => screen.Source is not null && screen.PresentedFrames > first + 3, "Monitor did not restart: " + mode, () => screen.PresentationError);
            Check(camera.PresentedGeneration == cameraGeneration, "Monitor toggle recreated the camera.");
            int blanks = 0;
            EventHandler observeBoth = (_, _) => { if (screen.Source is null || camera.Source is null) blanks++; };
            CompositionTarget.Rendering += observeBoth;
            long beforeScreen = screen.PresentedFrames, beforeCamera = camera.PresentedFrames;
            long screenBindings = screen.GpuSurfaceBindings, cameraBindings = camera.GpuSurfaceBindings;
            try { await Task.Delay(10000); }
            finally { CompositionTarget.Rendering -= observeBoth; }
            Check(blanks == 0 && screen.PresentedFrames > beforeScreen + 20 && camera.PresentedFrames > beforeCamera + 20,
                "Preview went blank or stalled: " + mode);
            Check(screen.PresentationError is null && camera.PresentationError is null, "Preview reported a failure: " + mode);
            Check(screen.GpuSurfaceBindings == screenBindings && camera.GpuSurfaceBindings == cameraBindings,
                "GPU preview rebound its WPF surface while presenting a stable source: " + mode);
            if (mode == RecordingExecutionMode.Automatic)
            {
                var monitor = LegacyGeometry.Describe(vm.SelectedScreen);
                ulong generation = screen.PresentedGeneration;
                await window.PreviewWorkspace.UpdateIdleAsync(monitor, vm.SelectedCameraFormat! with { DeviceId = "missing-ui-probe-camera" }, mode);
                await UntilAsync(() => camera.PresentationError is not null, "Camera failure was not reported.");
                Check(window.PreviewWorkspace.GetError(1) is null && screen.Source is not null && screen.PresentedGeneration == generation,
                    "Camera failure interrupted the monitor.");
                long previousFrames = camera.PresentedFrames;
                await window.PreviewWorkspace.UpdateIdleAsync(monitor, vm.SelectedCameraFormat, mode);
                await UntilAsync(() => camera.Source is not null && camera.PresentedFrames > previousFrames + 3,
                    "Camera preview did not recover.", () => window.PreviewWorkspace.GetError(2));
                Results.Add(new { name = "camera-failure-isolated-and-recovered" });
                window.WindowState = WindowState.Minimized;
                await Task.Delay(300);
                window.WindowState = WindowState.Normal;
                window.Activate();
                first = screen.PresentedFrames;
                previousFrames = camera.PresentedFrames;
                await UntilAsync(() => screen.PresentedFrames > first + 3 && camera.PresentedFrames > previousFrames + 3,
                    "Previews did not resume after restoring the window.", () => screen.PresentationError ?? camera.PresentationError);
                Results.Add(new { name = "previews-resume-after-minimize" });
            }
            Screenshot(window, "recorder-" + mode);
            Results.Add(new { name = "independent-previews", mode = mode.ToString(), camera = vm.SelectedWebcam.Name,
                screenFrames = screen.PresentedFrames - beforeScreen, cameraFrames = camera.PresentedFrames - beforeCamera, blanks });
            File.WriteAllText(Path.Combine(_output, "progress.json"), JsonSerializer.Serialize(Results));
        }
        string selectedCameraName = vm.SelectedWebcam.Name;
        foreach (var mode in Enum.GetValues<RecordingExecutionMode>())
        foreach (int sources in Enumerable.Range(1, 15).OrderBy(mask => System.Numerics.BitOperations.PopCount((uint)mask)).ThenBy(mask => mask))
        {
            if (!window.IsVisible)
            {
                foreach (Window open in app.Windows.Cast<Window>().ToArray()) open.Close();
                window = new RecordingScreenWindow();
                window.Show();
                vm = (RecordingScreenWindowVM)window.DataContext;
                await UntilAsync(() => vm.CanStartRecording, "Recorder did not reopen.");
                vm.SelectedWebcam = vm.Webcams.Single(camera => camera.Name == selectedCameraName);
                screen = (NativePreviewControl)window.FindName("ScreenNativePreview");
                camera = (NativePreviewControl)window.FindName("CameraNativePreview");
            }
            vm.SelectedExecutionMode = mode;
            vm.SelectedRecordingTimerSeconds = 0;
            Check(vm.SelectedMicrophone is not null && vm.SelectedRenderLoopback is not null,
                "The recording matrix requires a microphone and a system audio endpoint.");
            vm.IsScreenEnabled = (sources & 1) != 0;
            vm.IsWebcamEnabled = (sources & 2) != 0;
            vm.IsMicrophoneEnabled = (sources & 4) != 0;
            vm.IsSystemAudioEnabled = (sources & 8) != 0;
            await Task.Delay(1500);
            vm.HandleButtonAction(MenuAction.StartRecording);
            await Operation(vm, "_startTask").WaitAsync(TimeSpan.FromSeconds(120));
            Check(vm.CanPauseRecording && vm.RecordingError is null, $"Record command did not start ({mode}, sources {sources}): " + vm.RecordingError);
            await Task.Delay(8000);
            Check(vm.CanPauseRecording && vm.RecordingError is null,
                $"Recording stopped unexpectedly ({mode}, sources {sources}): " + vm.RecordingError);
            vm.HandleButtonAction(MenuAction.StopRecording);
            await Operation(vm, "_stopTask").WaitAsync(TimeSpan.FromSeconds(300));
            Check(vm.RecordingError is null && File.Exists(Path.Combine(vm.OutDirFolder, "manifest.json")), "Record command did not finalize: " + vm.RecordingError);
            Check(vm.OutDirFolder.StartsWith(app.Options.RecordingsDirectory + Path.DirectorySeparatorChar, StringComparison.OrdinalIgnoreCase), "Recording used the legacy protected folder.");
            int audioSources = (vm.IsMicrophoneEnabled ? 1 : 0) + (vm.IsSystemAudioEnabled ? 1 : 0);
            Check(Directory.GetFiles(vm.OutDirFolder, "*.wav").Count(path => new FileInfo(path).Length > 44) == audioSources,
                "Recording did not retain its selected audio tracks: " + mode);
            await VerifyMediaAsync(vm.OutDirFolder, sources);
            Results.Add(new { name = "record-and-finalize", mode = mode.ToString(), sources,
                monitor = vm.IsScreenEnabled, webcam = vm.IsWebcamEnabled,
                microphone = vm.IsMicrophoneEnabled, systemAudio = vm.IsSystemAudioEnabled,
                directory = vm.OutDirFolder });
            File.WriteAllText(Path.Combine(_output, "progress.json"), JsonSerializer.Serialize(Results));
        }
        await window.PreviewWorkspace.DisposeAsync();
        foreach (Window open in app.Windows.Cast<Window>().ToArray()) open.Close();
    }

    private static Task Operation(RecordingScreenWindowVM vm, string field) =>
        (Task)typeof(RecordingScreenWindowVM).GetField(field, BindingFlags.Instance | BindingFlags.NonPublic)!.GetValue(vm)!;

    private static async Task VerifyMediaAsync(string directory, int sources, double minimumSeconds = 7.5)
    {
        using var manifest = JsonDocument.Parse(await File.ReadAllTextAsync(Path.Combine(directory, "manifest.json")));
        var root = manifest.RootElement;
        Check(root.TryGetProperty("Metadata", out var metadata) && metadata.GetProperty("Version").GetInt32()==1 &&
              Directory.GetFiles(directory,"*.json",SearchOption.TopDirectoryOnly).Select(Path.GetFileName).SequenceEqual(["manifest.json"]),
              "A saved recording must have one consolidated manifest.json.");
        Check(!File.Exists(Path.Combine(directory,"final.mkv"))&&!File.Exists(Path.Combine(directory,"webcam.mp4"))&&
              !Directory.EnumerateFiles(directory,"*.mkv.job-*.json").Any()&&!Directory.EnumerateFiles(directory,"*.mp4.job-*.json").Any(),
              "Stop/Save automatically exported or converted recording tracks.");
        Check(root.GetProperty("State").GetString() == "Completed" && root.GetProperty("Errors").GetArrayLength() == 0,
            "The recording manifest contains a failure.");
        var assets = root.GetProperty("Assets").EnumerateArray().ToArray();
        string[] roles = ["screen", "camera", "microphone", "system"];
        var expected = roles.Where((_, index) => (sources & (1 << index)) != 0).Order().ToArray();
        Check(assets.Select(asset => asset.GetProperty("Role").GetString()!).Where(role => role != "primary").Order().SequenceEqual(expected),
            "Recorded tracks differ from selected sources.");
        if ((sources & 3) == 0)
            Check(Directory.GetFiles(directory, "preview.png.job-*.json").Length == 0,
                "An audio-only recording attempted to generate a video thumbnail.");
        var checks = new List<object>();
        foreach (var asset in assets)
        {
            string file = asset.GetProperty("File").GetString()!;
            string path = Path.GetFullPath(Path.Combine(directory, file));
            Check(path.StartsWith(directory + Path.DirectorySeparatorChar, StringComparison.OrdinalIgnoreCase), "Asset escaped its recording folder.");
            Check(asset.GetProperty("Finalized").GetBoolean() && new FileInfo(path).Length > 80, "Media was not finalized: " + file);
            long expectedHns = asset.GetProperty("DurationHns").GetInt64();
            var decodeClock = Stopwatch.StartNew();
            var info = new ProcessStartInfo(Path.Combine(AppContext.BaseDirectory, "Native", "FFmpeg", "ffmpeg.exe"))
            { UseShellExecute = false, CreateNoWindow = true, RedirectStandardOutput = true, RedirectStandardError = true };
            foreach (string argument in new[] { "-hide_banner", "-loglevel", "error", "-xerror", "-nostdin", "-threads", "2",
                "-i", path, "-map", "0", "-progress", "pipe:1", "-nostats", "-f", "null", "-" }) info.ArgumentList.Add(argument);
            using var process = Process.Start(info)!;
            var output = process.StandardOutput.ReadToEndAsync();
            var errors = process.StandardError.ReadToEndAsync();
            // Full-file validation scales with recording duration. A two-hour
            // 1080p track must not inherit a one-minute short-clip deadline.
            using var timeout = new CancellationTokenSource(TimeSpan.FromSeconds(
                Math.Clamp(expectedHns / (double)TimeSpan.TicksPerSecond / 5, 60, 1800)));
            try { await process.WaitForExitAsync(timeout.Token); }
            catch { if (!process.HasExited) process.Kill(entireProcessTree: true); throw; }
            string diagnostic = await errors;
            Check(process.ExitCode == 0 && string.IsNullOrWhiteSpace(diagnostic), "Media decoding failed: " + file + " " + diagnostic);
            string progress = await output;
            var values = progress.Split('\n').Select(line => line.Trim().Split('=', 2)).Where(pair => pair.Length == 2)
                .GroupBy(pair => pair[0]).ToDictionary(group => group.Key, group => group.Last()[1]);
            long decodedHns = long.Parse(values["out_time_us"], System.Globalization.CultureInfo.InvariantCulture) * 10;
            Check(decodedHns > TimeSpan.FromSeconds(minimumSeconds).Ticks && Math.Abs(decodedHns - expectedHns) < TimeSpan.FromMilliseconds(100).Ticks,
                $"Decoded duration differs from the manifest: {file}, {decodedHns} vs {expectedHns}");
            if (asset.GetProperty("Kind").GetString() == "video")
                Check(long.Parse(values["frame"], System.Globalization.CultureInfo.InvariantCulture) == asset.GetProperty("Video").GetProperty("FrameCount").GetInt64(),
                    "Decoded video frame count differs from the manifest: " + file);
            checks.Add(new { file, decodedHns, expectedHns, bytes = new FileInfo(path).Length,
                decodeWallSeconds = decodeClock.Elapsed.TotalSeconds });
            await File.WriteAllTextAsync(Path.Combine(directory, "media-verification-progress.json"),
                JsonSerializer.Serialize(new { status = "RUNNING", checks }));
        }
        await File.WriteAllTextAsync(Path.Combine(directory, "media-verification.json"), JsonSerializer.Serialize(new { status = "PASS", checks }));
    }
}

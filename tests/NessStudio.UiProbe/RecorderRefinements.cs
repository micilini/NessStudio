using System.IO;
using System.Windows;
using System.Windows.Controls;
using System.Windows.Threading;
using NAudio.CoreAudioApi;
using NAudio.Wave;
using NAudio.Wave.SampleProviders;
using NessStudio;
using NessStudio.Components.RecentProjects;
using NessStudio.Models;
using NessStudio.Recording;
using NessStudio.Recording.Engines;
using NessStudio.View.ExportScreen;
using NessStudio.View.HomeScreen;
using NessStudio.View.RecordingScreen;
using NessStudio.ViewModel;

internal static partial class Program
{
    private static async Task RecorderRefinementsAsync(App app)
    {
        await AudioMeterContractsAsync();
        long available = 10L * 1024 * 1024 * 1024;
        var factory = new BoundedSoakFactory(app.Services.RecorderFactory);
        var window = new RecordingScreenWindow(factory, _ => (ulong)Interlocked.Read(ref available));
        window.Show();
        var vm = (RecordingScreenWindowVM)window.DataContext;
        await UntilAsync(() => vm.CanStartRecording && vm.DiskSpaceMessage.Contains("10.0 GiB"), "Recorder did not initialize.");
        var footer = (TextBlock)window.FindName("DiskSpaceStatusText");
        foreach (var mode in Enum.GetValues<RecordingExecutionMode>())
        {
            vm.SelectedExecutionMode = mode;
            vm.SelectedRecordingFps = 60;
            await Task.Delay(60);
            Check(footer.Text == vm.FooterStatusMessage && footer.Text.Contains("60 FPS") && footer.Text.Contains("10.0 GiB"),
                "Footer did not immediately follow recording settings.");
        }
        Check(footer.TextAlignment == TextAlignment.Right && footer.TextWrapping == TextWrapping.NoWrap,
            "Recorder footer is not one right-aligned line.");
        vm.SelectedExecutionMode = RecordingExecutionMode.FullSoftware;
        vm.SelectedRecordingFps = 30;
        vm.SelectedRecordingTimerSeconds = 0;
        var settings = Descendants<MenuItem>(window).Single(item => Equals(item.Header, "Settings"));
        settings.ApplyTemplate();
        var header = (FrameworkElement)settings.Template.FindName("MenuHeader", settings);
        var headerBounds = header.TransformToAncestor(settings).TransformBounds(new Rect(header.RenderSize));
        Check(settings.ActualWidth < 85 && Math.Abs(headerBounds.Left - (settings.ActualWidth - headerBounds.Right)) < 1,
            "The Settings button still reserves uneven empty space.");
        Check(settings.Items.OfType<MenuItem>().Single(item => Equals(item.Header, "Recording mode")).ToolTip is null,
            "Recording mode still displays the removed hover explanation.");
        Interlocked.Exchange(ref available, 9L * 1024 * 1024 * 1024);
        await UntilAsync(() => footer.Text.Contains("9.0 GiB"), "Idle free-space status did not refresh.");
        Results.Add(new { name = "recorder-footer-live-settings-disk-space-and-compact-balanced-settings" });

        vm.IsScreenEnabled = true;
        vm.IsMicrophoneEnabled = true;
        vm.IsSystemAudioEnabled = true;
        Check(vm.SelectedMicrophone is not null && vm.SelectedRenderLoopback is not null, "Audio endpoints are required for the meter check.");
        var micSlider = (Slider)window.FindName("sldMicVolume");
        var systemSlider = (Slider)window.FindName("sldSystemVolume");
        var micMeter = (ProgressBar)window.FindName("MicrophoneSignalMeter");
        var systemMeter = (ProgressBar)window.FindName("SystemAudioSignalMeter");
        window.UpdateLayout();
        foreach (var (slider, meter) in new[] { (micSlider, micMeter), (systemSlider, systemMeter) })
        {
            Check(meter.IsVisible && meter.ActualHeight <= 6 && Math.Abs(meter.ActualWidth - slider.ActualWidth) < 1,
                "An audio meter is missing or does not match its slider width.");
            var sliderBounds = slider.TransformToAncestor(window).TransformBounds(new Rect(slider.RenderSize));
            var meterBounds = meter.TransformToAncestor(window).TransformBounds(new Rect(meter.RenderSize));
            Check(meterBounds.Top >= sliderBounds.Bottom && Math.Abs(meterBounds.Left - sliderBounds.Left) < 1 &&
                new Rect(window.RenderSize).Contains(meterBounds), "An audio meter overlaps or clips its slider.");
        }
        using var output = new WasapiOut(vm.SelectedRenderLoopback, AudioClientShareMode.Shared, true, 50);
        var tone = new SignalGenerator(48000, 2) { Type = SignalGeneratorType.Sin, Frequency = 660, Gain = .03 };
        output.Init(tone.ToWaveProvider());
        output.Play();
        systemSlider.Value = 0;
        micSlider.Value = 0;
        await UntilAsync(() => vm.SystemAudioSignalLevel > .2, "System audio meter did not respond with its recording slider at zero.");
        double zeroGainPeak = vm.SystemAudioSignalLevel;
        systemSlider.Value = 100;
        await Task.Delay(300);
        Check(vm.SystemAudioSignalLevel > .2, "System meter stopped following the raw endpoint signal.");
        systemSlider.Value = 0;
        Screenshot(window, "recorder-settings-with-meters");
        double idleHeight = window.ActualHeight;
        await StartAsync(vm);
        await UntilAsync(() => window.ActualHeight < idleHeight - 80,
            "Active window did not shrink.");
        window.UpdateLayout();
        Check(window.ActualHeight < idleHeight - 80 && footer.IsVisible && footer.Text.Contains("CPU") && footer.Text.Contains("30 FPS"),
            "Active recording did not use the compact layout and shared footer.");
        var transport = (Grid)window.FindName("GridRecord");
        var buttons = Descendants<Button>(transport).OrderBy(button => button.TransformToAncestor(transport).Transform(new Point()).X).ToArray();
        Check(buttons.Length == 2, "Pause and Stop buttons were not found.");
        foreach (var button in buttons)
            foreach (var label in Descendants<TextBlock>(button))
                Check(new Rect(button.RenderSize).Contains(label.TransformToAncestor(button).TransformBounds(new Rect(label.RenderSize))),
                    "A transport icon pushed its Pause or Stop label outside the button.");
        Rect pauseBounds = buttons[0].TransformToAncestor(transport).TransformBounds(new Rect(buttons[0].RenderSize));
        Rect stopBounds = buttons[1].TransformToAncestor(transport).TransformBounds(new Rect(buttons[1].RenderSize));
        Check(stopBounds.Left - pauseBounds.Right >= 18 && transport.Margin.Right >= 18,
            "Active controls do not have horizontal breathing room.");
        Interlocked.Exchange(ref available, 8L * 1024 * 1024 * 1024);
        await UntilAsync(() => footer.Text.Contains("8.0 GiB"), "Active free-space status did not refresh.");
        Check(Field(vm, "_audioMonitor") is null && vm.SystemAudioSignalLevel == 0 && vm.RecordingError is null,
            "The idle audio meter retained its capture endpoint during native recording.");
        await Task.Delay(2200);
        Screenshot(window, "recorder-active-compact");
        vm.HandleButtonAction(MenuAction.PauseRecording);
        await UntilAsync(() => vm.CurrentRecordingState == RecordingState.Paused, "Recording did not pause.");
        Check(Field(vm, "_audioMonitor") is null, "An idle microphone stream reopened while the recorder was paused.");
        Screenshot(window, "recorder-paused-compact");
        vm.HandleButtonAction(MenuAction.PauseRecording);
        await UntilAsync(() => vm.CurrentRecordingState == RecordingState.Recording, "Recording did not resume.");
        await Task.Delay(1200);
        output.Stop();
        // Other apps may continue playback; no-packet expiry is covered by the isolated signal checks.
        vm.HandleButtonAction(MenuAction.StopRecording);
        await Operation(vm, "_stopTask").WaitAsync(TimeSpan.FromSeconds(150));
        Check(vm.RecordingError is null && ProjectFilesValidation.Check(vm.OutDirFolder).IsValid,
            "Recording with live meters did not save intact media: " + vm.RecordingError);
        foreach (var asset in ProjectManifestStore.Assets(ProjectManifestStore.Read(vm.OutDirFolder).Manifest))
            await DecodeExportAsync(ProjectManifestStore.ResolvePath(vm.OutDirFolder, asset.File));
        Results.Add(new { name = "idle-raw-audio-meters-hand-off-endpoints-before-native-record-pause-resume-save", zeroGainPeak,
            idleHeight, activeHeight = 221, directory = vm.OutDirFolder });
        await window.PreviewWorkspace.DisposeAsync();
        foreach (Window open in app.Windows.Cast<Window>().ToArray()) open.Close();
        await MissingProjectActionsAsync(app, vm.OutDirFolder);
    }

    private static async Task MissingProjectActionsAsync(App app, string intactProject)
    {
        var home = new HomeScreenWindow();
        home.Show();
        var recent = (RecentProjectsControl)((HomeScreenWindowVM)home.DataContext).ProjectsContent;
        await UntilAsync(() => !recent.RecentProjectsControlVM.IsLoadingProjectBlockVisible, "Project list did not load.");
        foreach (var (name, action) in new[] { ("missing-folder", "Open"), ("missing-track", "Export"), ("damaged-track", "Export") })
        {
            string directory = Path.Combine(_output, name);
            if (name != "missing-folder")
            {
                Directory.CreateDirectory(directory);
                foreach (string path in Directory.EnumerateFiles(intactProject)) File.Copy(path, Path.Combine(directory, Path.GetFileName(path)));
                string track = Path.Combine(directory, "mic.wav");
                if (name == "missing-track") File.Delete(track);
                else
                {
                    using var stream = new FileStream(track, FileMode.Open, FileAccess.Write);
                    stream.Write(new byte[12]); // Same length, destroyed WAV header.
                }
            }
            var model = new ProjectsModel { Title = name, ProjectFolderPath = directory,
                ProjectFilePath = Path.Combine(directory, "manifest.json") };
            app.DBConnection.Insert(model);
            await recent.RecentProjectsControlVM.ReloadAsync();
            var card = Descendants<Border>(recent).Single(border => Equals(border.Tag, directory));
            Check(app.DBConnection.Find<ProjectsModel>(model.Id) is not null, "Invalid projects were removed during list loading.");
            var menuButton = Descendants<Button>(card).Single(button => Equals(button.ToolTip, "More options"));
            menuButton.RaiseEvent(new RoutedEventArgs(Button.ClickEvent));
            var menu = menuButton.ContextMenu;
            var item = menu.Items.OfType<MenuItem>().Single(entry =>
                ((StackPanel)entry.Header).Children.OfType<TextBlock>().Any(text => text.Text == action));
            menu.IsOpen = false;
            bool acknowledged = false;
            Exception? dialogError = null;
            var dismiss = new DispatcherTimer { Interval = TimeSpan.FromMilliseconds(100) };
            dismiss.Tick += (_, _) =>
            {
                var dialog = app.Windows.OfType<Window>().FirstOrDefault(open => Equals(open.Tag, "EnglishMessageBox"));
                if (dialog is null) return;
                dismiss.Stop();
                try
                {
                    Check(app.DBConnection.Find<ProjectsModel>(model.Id) is not null && card.IsVisible,
                        "Project was removed before the user acknowledged the error.");
                    Check(!app.Windows.OfType<ExportScreenWindow>().Any(), "Export opened before validating the recording.");
                    var ok = Descendants<Button>(dialog).Single(button => Equals(button.Content, "OK"));
                    Screenshot(dialog, "project-files-error-" + name);
                    acknowledged = true;
                    ok.RaiseEvent(new RoutedEventArgs(Button.ClickEvent));
                }
                catch (Exception error) { dialogError = error; dialog.Close(); }
            };
            dismiss.Start();
            try
            {
                item.RaiseEvent(new RoutedEventArgs(MenuItem.ClickEvent));
                await UntilAsync(() => dialogError is not null || acknowledged && app.DBConnection.Find<ProjectsModel>(model.Id) is null,
                    "Invalid recording did not show its error and leave the library after OK.");
                if (dialogError is not null) throw dialogError;
                Check(!Descendants<Border>(recent).Any(border => Equals(border.Tag, directory)), "Invalid project card remained after OK.");
                if (name != "missing-folder") Check(File.Exists(Path.Combine(directory, "system.wav")), "Automatic cleanup deleted surviving media.");
                await recent.RecentProjectsControlVM.ReloadAsync();
                Check(!Descendants<Border>(recent).Any(border => Equals(border.Tag, directory)), "Removed card returned after list reload.");
                Results.Add(new { name = "project-files-validation-before-" + action, scenario = name, acknowledgedBeforeRemoval = acknowledged });
            }
            finally { dismiss.Stop(); }
        }
        home.Close();
    }
}

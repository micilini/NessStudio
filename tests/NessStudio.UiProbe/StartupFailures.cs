using System.IO;
using System.Windows;
using System.Windows.Controls;
using System.Windows.Threading;
using NessStudio;
using NessStudio.Models;
using NessStudio.Recording.Engines;
using NessStudio.View.RecordingScreen;
using NessStudio.View.SavingRecordingScreen;
using NessStudio.ViewModel;

internal static partial class Program
{
    private sealed class StartupFactory(RecordingCapabilities capabilities) : IRecorderEngineFactory
    {
        public RecordingCapabilities Capabilities => capabilities;
        public StartupService? Service { get; private set; }
        public IRecordingService Create(RecordingRequest _) => Service = new(capabilities);
    }

    private sealed class StartupService(RecordingCapabilities capabilities) : IRecordingService
    {
        public event EventHandler<RecordingStateChangedEventArgs>? StateChanged;
        public RecordingCapabilities Capabilities => capabilities;
        public RecordingStatistics Statistics => new(null, null, null);
        public RecordingResult? Result => null;
        public RecordingState State { get; private set; }
        public readonly TaskCompletionSource StartGate = new(TaskCreationOptions.RunContinuationsAsynchronously);
        public readonly TaskCompletionSource CleanupGate = new(TaskCreationOptions.RunContinuationsAsynchronously);
        public bool StartEntered, CleanupEntered, Cancelled, SaveCalled;
        private Task? _cleanup;
        private void Set(RecordingState state) { State = state; StateChanged?.Invoke(this, new(state)); }
        public Task PrepareAsync() { Set(RecordingState.Ready); return Task.CompletedTask; }
        public async Task StartAsync() { StartEntered = true; Set(RecordingState.Starting); await StartGate.Task; Set(RecordingState.Recording); }
        public void CancelStartup() { Cancelled = true; StartGate.TrySetCanceled(); CleanupGate.TrySetResult(); }
        public Task PauseAsync() => Task.CompletedTask;
        public Task ResumeAsync() => Task.CompletedTask;
        public Task StopAsync() => Task.CompletedTask;
        public Task<string> StopAndFinalizeAsync(IProgress<RecordingSaveProgress>? progress = null)
        { SaveCalled = true; throw new InvalidOperationException("A failed startup must not enter saving."); }
        public ValueTask DisposeAsync() => new(_cleanup ??= CleanupAsync());
        private async Task CleanupAsync() { CleanupEntered = true; await CleanupGate.Task; Set(RecordingState.Disposed); }
        public void Dispose() { CleanupGate.TrySetResult(); _ = DisposeAsync(); }
    }

    private static async Task StartupFailuresAsync(App app)
    {
        foreach (bool closeDuringStart in new[] { false, true })
        {
            var factory = new StartupFactory(app.Services.RecorderFactory.Capabilities);
            var window = new RecordingScreenWindow(factory);
            window.Show();
            var vm = (RecordingScreenWindowVM)window.DataContext;
            await UntilAsync(() => vm.CanStartRecording, "Startup recorder did not load.");
            vm.IsMicrophoneEnabled = true;
            vm.SelectedRecordingTimerSeconds = 0;
            vm.HandleButtonAction(MenuAction.StartRecording);
            await UntilAsync(() => factory.Service?.StartEntered == true, "Scripted startup was not entered.");
            var service = factory.Service!;
            Check(vm.IsSettingsPanelVisible && !vm.IsRecordingPanelVisible && !vm.CanPauseRecording && !vm.CanStopRecording,
                "An unconfirmed startup showed the recording timer or enabled transport controls.");
            Check(Field(vm, "_audioMonitor") is null && vm.MicrophoneSignalLevel == 0,
                "The idle meter retained its microphone capture while the recorder started.");
            if (closeDuringStart)
            {
                window.Close();
                await Operation(vm, "_startTask").WaitAsync(TimeSpan.FromSeconds(8));
                await UntilAsync(() => !window.IsVisible, "Closing the recorder waited forever for an uncancelled Start.", seconds: 8);
                Check(service.Cancelled && !service.SaveCalled, "Close did not interrupt startup or tried to save nonexistent media.");
                Results.Add(new { name = "window-close-cancels-pending-start-without-saving" });
            }
            else
            {
                bool shown = false;
                Exception? dialogFailure = null;
                var dismiss = new DispatcherTimer { Interval = TimeSpan.FromMilliseconds(100) };
                dismiss.Tick += (_, _) =>
                {
                    var dialog = app.Windows.OfType<Window>().FirstOrDefault(open => Equals(open.Tag, "EnglishMessageBox"));
                    if (dialog is null) return;
                    dismiss.Stop();
                    try
                    {
                        Check(service.CleanupEntered && !service.CleanupGate.Task.IsCompleted,
                            "The startup error waited for device cleanup before becoming visible.");
                        Check(!app.Windows.OfType<SavingRecordingWindow>().Any(), "A failed Start opened the saving window.");
                        Check(Descendants<TextBlock>(dialog).Any(text => text.Text.Contains("Simulated audio startup fault")),
                            "The startup error lost the original device failure.");
                        shown = true;
                        Screenshot(dialog, "startup-failure-before-device-cleanup");
                        Descendants<Button>(dialog).Single(button => Equals(button.Content, "OK")).RaiseEvent(new RoutedEventArgs(Button.ClickEvent));
                    }
                    catch (Exception error) { dialogFailure = error; dialog.Close(); }
                    finally { service.CleanupGate.TrySetResult(); }
                };
                dismiss.Start();
                try
                {
                    service.StartGate.TrySetException(new IOException("Simulated audio startup fault"));
                    await Operation(vm, "_startTask").WaitAsync(TimeSpan.FromSeconds(8));
                    if (dialogFailure is not null) throw dialogFailure;
                    Check(shown && !service.SaveCalled && vm.CanStartRecording,
                        "Startup failure was hidden, entered saving, or prevented a subsequent attempt.");
                    window.Close();
                    await UntilAsync(() => !window.IsVisible, "Failed recorder did not close.", seconds: 8);
                    Results.Add(new { name = "startup-error-is-visible-before-slow-cleanup-and-recorder-can-retry" });
                }
                finally { dismiss.Stop(); service.CleanupGate.TrySetResult(); }
            }
        }
    }
}

using System.IO;
using System.Reflection;
using System.Text.Json;
using System.Windows;
using System.Windows.Controls;
using NessStudio;
using NessStudio.Components.Menu;
using NessStudio.Models;
using NessStudio.Recording;
using NessStudio.Recording.Engines;
using NessStudio.ViewModel;

internal static partial class Program
{
    private static async Task RecordingQualityAsync(App app)
    {
        Check(app.Services.RecordingPreferences.Load().QualityPreset == RecordingQualityPreset.High,
            "A new profile did not default to high recording quality.");
        var noticeMethod = typeof(NativeRecorderEngineFactory).Assembly
            .GetType("NessStudio.Recording.Engines.NativeRecordingService")!
            .GetMethod("AudioRecoveryNotice", BindingFlags.NonPublic | BindingFlags.Static)!;
        foreach (var (missing, inserted, shouldWarn) in new[] { (48, 48, false), (48000, 0, false), (4799, 4799, false), (4800, 4800, true) })
        {
            var snapshot = JsonSerializer.SerializeToElement(new { metrics = new { audio = new[] {
                new { recoveredAudioGaps = new { missingInputFrames = missing, insertedSilentOutputFrames = inserted } } } } });
            Check((noticeMethod.Invoke(null, [snapshot]) is string) == shouldWarn,
                "A tiny/paused correction showed a notice, or significant saved silence was hidden.");
        }
        Results.Add(new { name = "save-notice-counts-only-significant-silence-in-saved-audio" });

        foreach (var mode in Enum.GetValues<RecordingExecutionMode>())
        {
            foreach (var quality in Enum.GetValues<RecordingQualityPreset>())
            {
                var window = await NewRecorderAsync(app, mode, 1);
                var vm = (RecordingScreenWindowVM)window.DataContext;
                vm.SelectedRecordingFps = 30;
                var settings = Descendants<MenuItem>(window).Single(item => Equals(item.Header, "Settings"));
                var qualityMenu = settings.Items.OfType<MenuItem>().Single(item => Equals(item.Header, "Recording quality"));
                if (mode == RecordingExecutionMode.Automatic && quality == RecordingQualityPreset.Balanced)
                {
                    settings.IsSubmenuOpen = qualityMenu.IsSubmenuOpen = true;
                    await Task.Delay(200);
                    Screenshot(window, "recording-quality-settings");
                    qualityMenu.IsSubmenuOpen = settings.IsSubmenuOpen = false;
                }
                Check(vm.QualityOptions.Count == 3, "This recording route did not expose its supported quality choices.");
                vm.SelectQualityOptionCommand.Execute((int)quality);
                Check(vm.SelectedQualityPreset == quality && app.Services.RecordingPreferences.Load().QualityPreset == quality &&
                    vm.FooterStatusMessage.Contains(quality + " quality") && vm.QualityOptions.Single(option => option.IsSelected).Value == (int)quality,
                    "The selected quality did not update its menu, footer and persisted preference.");
                var monitor = vm.ScreenOptions.First(option => option.Value?.Primary == true).Value!;
                typeof(RecordingScreenWindowVM).GetMethod("ApplyDrawArea", BindingFlags.NonPublic | BindingFlags.Instance)!
                    .Invoke(vm, [new Rect(monitor.Bounds.X + 120, monitor.Bounds.Y + 100, 512, 512)]);
                await StartAsync(vm);
                vm.SelectQualityOptionCommand.Execute((int)(quality == RecordingQualityPreset.High ? RecordingQualityPreset.Balanced : RecordingQualityPreset.High));
                Check(vm.SelectedQualityPreset == quality, "Recording quality changed after the encoder was opened.");
                await Task.Delay(3600);
                await FinishAsync(app, window, 1);
                CheckRecordedGeometry(vm.OutDirFolder, 512, 512, quality);
                Results.Add(new { name = "512-square-remains-native-sized-at-every-quality", mode = mode.ToString(), quality = quality.ToString(), directory = vm.OutDirFolder });
            }

            var fullWindow = await NewRecorderAsync(app, mode, 15);
            var fullVm = (RecordingScreenWindowVM)fullWindow.DataContext;
            Check(fullVm.SelectedQualityPreset == RecordingQualityPreset.Maximum, "A reopened recorder lost the selected quality.");
            fullVm.SelectedRecordingFps = 30;
            fullVm.SelectQualityOptionCommand.Execute((int)RecordingQualityPreset.High);
            fullVm.SelectedScreenOption = fullVm.ScreenOptions.First(option => option.Value?.Bounds.Width == 1920 && option.Value.Bounds.Height == 1080);
            var selectedCamera = fullVm.SelectedCameraFormat!;
            await StartAsync(fullVm);
            await Task.Delay(2000);
            fullVm.HandleButtonAction(MenuAction.PauseRecording);
            await UntilAsync(() => fullVm.CurrentRecordingState == RecordingState.Paused, "Full HD recording did not pause.", () => fullVm.RecordingError);
            await Task.Delay(450);
            fullVm.HandleButtonAction(MenuAction.PauseRecording);
            await UntilAsync(() => fullVm.CurrentRecordingState == RecordingState.Recording, "Full HD recording did not resume.", () => fullVm.RecordingError);
            await Task.Delay(2000);
            await FinishAsync(app, fullWindow, 15);
            CheckRecordedGeometry(fullVm.OutDirFolder, 1920, 1080, RecordingQualityPreset.High);
            using var fullManifest = JsonDocument.Parse(File.ReadAllText(Path.Combine(fullVm.OutDirFolder, "manifest.json")));
            var camera = fullManifest.RootElement.GetProperty("Assets").EnumerateArray().Single(asset => asset.GetProperty("Role").GetString() == "camera").GetProperty("Video");
            Check(camera.GetProperty("Width").GetInt32() == selectedCamera.Width && camera.GetProperty("Height").GetInt32() == selectedCamera.Height,
                "The independent camera track was reduced or enlarged relative to its selected input.");
            Results.Add(new { name = "full-hd-screen-and-native-camera-with-four-sources-pause-and-save", mode = mode.ToString(), directory = fullVm.OutDirFolder });
        }
    }

    private static void CheckRecordedGeometry(string directory, int width, int height, RecordingQualityPreset quality)
    {
        using var manifest = JsonDocument.Parse(File.ReadAllText(Path.Combine(directory, "manifest.json")));
        var video = manifest.RootElement.GetProperty("Assets").EnumerateArray().Single(asset => asset.GetProperty("Role").GetString() == "screen").GetProperty("Video");
        Check(video.GetProperty("Width").GetInt32() == width && video.GetProperty("Height").GetInt32() == height,
            "Compression quality changed the resolution of the captured source.");
        using var native = ProjectMetadataStore.ReadDocument(directory, "recording-native.json")!;
        var encoder = native.RootElement.GetProperty("snapshot").GetProperty("metrics").GetProperty("encoder");
        Check(encoder.GetProperty("quality").GetString() == quality.ToString() &&
            encoder.GetProperty("profile").GetInt32() == (quality == RecordingQualityPreset.Balanced ? 66 : 100),
            "The saved encoder did not use the requested quality and AVC profile.");
    }
}

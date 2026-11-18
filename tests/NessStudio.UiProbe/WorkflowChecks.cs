using System.Diagnostics;
using System.IO;
using System.Reflection;
using System.Runtime.InteropServices;
using System.Windows;
using System.Windows.Controls;
using System.Windows.Controls.Primitives;
using MaterialDesignThemes.Wpf;
using NessStudio;
using NessStudio.Components.RecentProjects;
using NessStudio.Models;
using NessStudio.View.AboutScreen;
using NessStudio.View.ExportScreen;
using NessStudio.View.HomeScreen;
using NessStudio.ViewModel;
using NessStudio.ViewModel.Helpers;

internal static partial class Program
{
    private static async Task WorkflowAsync(App app, string source)
    {
        Check(app.Services.NativeEngineError is null && app.Services.NativeEngineBuild is not null,
            "The packaged native engine did not load: " + app.Services.NativeEngineError);
        Results.Add(new { name = "runtime-and-native-bootstrap", baseDirectory = AppContext.BaseDirectory,
            processPath = Environment.ProcessPath, runtimeDirectory = RuntimeEnvironment.GetRuntimeDirectory(),
            nativeBuild = app.Services.NativeEngineBuild });
        app.DebugLogsEnabled = false;
        var about = new AboutScreenWindow();
        about.Show();
        var toggle = (CheckBox)about.FindName("EnableDebugLogsCheckBox");
        Check(toggle.IsChecked == false, "About did not show the disabled log preference.");
        toggle.IsChecked = true;
        Check(app.DebugLogsEnabled && DatabaseHelper.QuerySingle<SettingsModel>("SELECT * FROM Settings WHERE Id = ?", 1).EnableDebugLogs,
            "About did not persist the enabled log preference.");
        await Task.Run(() => DebugLog.Write("workflow-enabled-marker"));
        DebugLog.Flush();
        Check(ReadShared(DebugLog.GetPath()).Contains("workflow-enabled-marker"), "Enabled logs were not written.");
        toggle.IsChecked = false;
        Check(!app.DebugLogsEnabled && !DatabaseHelper.QuerySingle<SettingsModel>("SELECT * FROM Settings WHERE Id = ?", 1).EnableDebugLogs,
            "About did not persist the disabled log preference.");
        long disabledLength = new FileInfo(DebugLog.GetPath()).Length;
        await Task.Run(() => { for (int i = 0; i < 100; i++) DebugLog.Logger.Write("workflow-disabled-marker"); });
        DebugLog.WriteError("workflow-error-marker");
        DebugLog.Flush();
        Check(new FileInfo(DebugLog.GetPath()).Length == disabledLength && !ReadShared(DebugLog.GetPath()).Contains("workflow-disabled-marker"),
            "Disabling logs still appended diagnostic messages.");
        Check(ReadShared(DebugLog.GetErrorPath()).Contains("workflow-error-marker"), "Failure logging was disabled with diagnostics.");
        about.Close();
        about = new AboutScreenWindow();
        about.Show();
        Check(((CheckBox)about.FindName("EnableDebugLogsCheckBox")).IsChecked == false, "Reopening About lost the log preference.");
        ((CheckBox)about.FindName("EnableDebugLogsCheckBox")).BringIntoView();
        about.UpdateLayout();
        Screenshot(about, "about-disabled-logs");
        about.Close();
        Results.Add(new { name = "about-log-policy-persisted", backgroundWritesSuppressed = 100, separateErrorLog = true });

        var document = NessStudio.Recording.ProjectManifestStore.Read(source);
        var model = new ProjectsModel { Title = "Export verification", ProjectFolderPath = source,
            ProjectFilePath = document.AuthorityPath, UpdatedAt = DateTime.Now,
            ThumbnailPath = Path.Combine(source, "preview.png") };
        var home = new HomeScreenWindow();
        home.Show();
        var recent = (RecentProjectsControl)((HomeScreenWindowVM)home.DataContext).ProjectsContent;
        await UntilAsync(() => !recent.RecentProjectsControlVM.IsLoadingProjectBlockVisible, "Recent recordings did not load.");
        recent.RecentProjectsControlVM.IsNoneProjectBlockVisible = false;
        recent.RecentProjectsControlVM.IsProjectBlockVisible = true;
        string[] titles = [model.Title, Path.GetFileName(source), "Screen recording with webcam, microphone and system audio - " + new string('W', 120)];
        for (int index = 0; index < titles.Length; index++)
        {
            model.Title = titles[index];
            await recent.RenderProjectsAsync([model]);
            home.UpdateLayout();
            await Task.Delay(250);
            var menu = Descendants<Button>(recent).Single(button => Equals(button.ToolTip, "More options"));
            var icon = (PackIcon)menu.Content;
            Point center = icon.TranslatePoint(new Point(icon.ActualWidth / 2, icon.ActualHeight / 2), menu);
            Check(Math.Abs(center.X - menu.ActualWidth / 2) < 1 && Math.Abs(center.Y - menu.ActualHeight / 2) < 1,
                "The recording options icon is not centered.");
            var card = Descendants<Border>(recent).Single(border => Equals(border.Tag, source));
            Rect cardBounds = new Rect(card.RenderSize);
            Rect menuBounds = menu.TransformToAncestor(card).TransformBounds(new Rect(menu.RenderSize));
            Check(cardBounds.Contains(menuBounds), "A recording title pushed its menu outside the card.");
            var title = Descendants<TextBlock>(card).Single(text => text.Text == model.Title);
            Rect titleBounds = title.TransformToAncestor(card).TransformBounds(new Rect(title.RenderSize));
            Check(!titleBounds.IntersectsWith(menuBounds) && Math.Abs(titleBounds.Top + titleBounds.Height / 2 - menuBounds.Top - menuBounds.Height / 2) < 1,
                "The menu overlaps the recording title or is no longer beside it.");
            foreach (var label in Descendants<TextBlock>(card))
                Check(cardBounds.Contains(label.TransformToAncestor(card).TransformBounds(new Rect(label.RenderSize))),
                    "Recording metadata is clipped by the card.");
            Screenshot(home, "recording-options-" + index);
            menu.RaiseEvent(new RoutedEventArgs(ButtonBase.ClickEvent));
            Check(menu.ContextMenu?.IsOpen == true, "The recording options menu did not open.");
            menu.ContextMenu!.IsOpen = false;
        }
        model.Title = titles[0];
        home.Close();
        Results.Add(new { name = "recording-options-contained-beside-title-and-clickable", titleLengths = titles.Select(title => title.Length).ToArray() });

        var export = new ExportScreenWindow(model);
        export.Show();
        await Task.Delay(250);
        var output = (TextBox)export.FindName("OutputPathTextBox");
        var open = (Button)export.FindName("OpenOutputFolderButton");
        string singleFolder = Path.Combine(_output, "export single & ç");
        string single = Path.Combine(singleFolder, "video.mp4");
        output.Text = single;
        open.RaiseEvent(new RoutedEventArgs(ButtonBase.ClickEvent));
        await UntilAsync(() => ExplorerFolder(singleFolder), "Open Folder did not open the selected destination.");
        ExplorerFolder(singleFolder, close: true);
        await UntilAsync(() => !ExplorerFolder(singleFolder), "The owned Explorer window did not close.");
        open.BringIntoView();
        export.UpdateLayout();
        Screenshot(export, "export-open-folder");
        await ExportFromWindowAsync(export);
        Check(File.Exists(single), "The single-file export is missing.");
        await UntilAsync(() => ExplorerFolder(singleFolder), "Successful export did not open its folder automatically.");
        await DecodeExportAsync(single);
        ExplorerFolder(singleFolder, close: true);
        Results.Add(new { name = "single-export-opens-folder-after-success", file = single });

        typeof(ExportScreenWindow).GetMethod("SetExportMode", BindingFlags.Instance | BindingFlags.NonPublic)!
            .Invoke(export, [RecordingExportMode.SeparateTracks]);
        string tracks = Path.Combine(_output, "export tracks");
        output.Text = tracks;
        ((CheckBox)export.FindName("ExportMicrophoneCheckBox")).IsChecked = true;
        ((CheckBox)export.FindName("ExportSystemCheckBox")).IsChecked = true;
        await ExportFromWindowAsync(export);
        await UntilAsync(() => ExplorerFolder(tracks), "Successful track export opened the wrong folder.");
        string[] expected = ["screen.mp4", "webcam.mp4", "microphone.wav", "system.wav", "audio_mix.wav"];
        foreach (string file in expected) await DecodeExportAsync(Path.Combine(tracks, file));
        ExplorerFolder(tracks, close: true);
        ((ProgressBar)export.FindName("ExportProgressBar")).BringIntoView();
        export.UpdateLayout();
        Screenshot(export, "export-completed");
        export.Close();
        DebugLog.Flush();
        Check(new FileInfo(DebugLog.GetPath()).Length == disabledLength, "Normal export operations wrote disabled debug logs.");
        Results.Add(new { name = "separate-export-opens-folder-after-success", directory = tracks, files = expected });
    }

    private static string ReadShared(string path)
    {
        using var input = new FileStream(path, FileMode.Open, FileAccess.Read, FileShare.ReadWrite);
        using var reader = new StreamReader(input);
        return reader.ReadToEnd();
    }

    private static async Task ExportFromWindowAsync(ExportScreenWindow window)
    {
        var operation = (Task)typeof(ExportScreenWindow).GetMethod("StartExportAsync", BindingFlags.Instance | BindingFlags.NonPublic)!.Invoke(window, null)!;
        await operation.WaitAsync(TimeSpan.FromSeconds(60));
        var progress = (ProgressBar)window.FindName("ExportProgressBar");
        Check(progress.IsVisible && !progress.IsIndeterminate && progress.Value == 100,
            "A completed export did not show 100%.");
        Check(((Button)window.FindName("BrowseOutputButton")).IsEnabled, "Browse remained disabled after export.");
        Check(((TextBlock)window.FindName("ExportStatusTextBlock")).Text.StartsWith("100% - ", StringComparison.Ordinal),
            "The completed export status was not retained.");
    }

    private static async Task DecodeExportAsync(string path)
    {
        Check(File.Exists(path), "An exported track is missing: " + path);
        var info = new ProcessStartInfo(ExportService.FindFFmpegPath())
        { UseShellExecute = false, CreateNoWindow = true, RedirectStandardError = true };
        foreach (string argument in new[] { "-v", "error", "-xerror", "-nostdin", "-threads", "2", "-i", path, "-map", "0", "-f", "null", "-" })
            info.ArgumentList.Add(argument);
        using var process = Process.Start(info)!;
        var diagnostic = process.StandardError.ReadToEndAsync();
        using var timeout = new CancellationTokenSource(TimeSpan.FromSeconds(30));
        try { await process.WaitForExitAsync(timeout.Token); }
        catch { if (!process.HasExited) process.Kill(entireProcessTree: true); throw; }
        string errors = await diagnostic;
        Check(process.ExitCode == 0 && string.IsNullOrWhiteSpace(errors), "Export failed full decoding: " + errors);
    }

    private static bool ExplorerFolder(string directory, bool close = false)
    {
        dynamic shell = Activator.CreateInstance(Type.GetTypeFromProgID("Shell.Application")!)!;
        dynamic windows = shell.Windows();
        bool found = false;
        try
        {
            int count = windows.Count;
            for (int index = count - 1; index >= 0; index--)
            {
                dynamic? window = windows.Item(index);
                if (window is null) continue;
                try
                {
                    string path = (string)window.Document.Folder.Self.Path;
                    if (!Path.IsPathFullyQualified(path) ||
                        !string.Equals(Path.GetFullPath(path).TrimEnd(Path.DirectorySeparatorChar),
                            directory.TrimEnd(Path.DirectorySeparatorChar), StringComparison.OrdinalIgnoreCase)) continue;
                    found = true;
                    if (close) window.Quit();
                }
                catch (COMException) { }
                finally { Marshal.ReleaseComObject(window); }
            }
        }
        finally { Marshal.ReleaseComObject(windows); Marshal.ReleaseComObject(shell); }
        return found;
    }
}

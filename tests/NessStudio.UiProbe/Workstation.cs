using System.IO;
using System.Reflection;
using System.Runtime.InteropServices;
using System.Text.Json;
using System.Windows;
using System.Windows.Controls;
using System.Windows.Interop;
using System.Windows.Media;
using System.Windows.Media.Imaging;
using NessStudio;
using NessStudio.Presentation;
using NessStudio.Recording.Engines;
using NessStudio.View.RecordingScreen;
using NessStudio.ViewModel;

internal static partial class Program
{
    [DllImport("user32.dll")] private static extern nint GetDC(nint window);
    [DllImport("user32.dll")] private static extern int ReleaseDC(nint window, nint dc);
    [DllImport("gdi32.dll")] private static extern uint GetPixel(nint dc, int x, int y);
    [DllImport("user32.dll")] private static extern bool SetWindowPos(nint window, nint after, int x, int y, int w, int h, uint flags);
    [DllImport("user32.dll")] private static extern bool GetWindowRect(nint window, out NativeRect rect);
    [DllImport("user32.dll")] private static extern nint WindowFromPoint(NativePoint point);
    [DllImport("user32.dll")] private static extern bool GetWindowDisplayAffinity(nint window, out uint affinity);
    [DllImport("dwmapi.dll")] private static extern int DwmGetWindowAttribute(nint window, uint attribute, out NativeRect rect, int bytes);
    [DllImport("dwmapi.dll")] private static extern int DwmGetWindowAttribute(nint window, uint attribute, out uint value, int bytes);
    [StructLayout(LayoutKind.Sequential)] private struct NativePoint { public int X, Y; }
    [StructLayout(LayoutKind.Sequential)] private struct NativeRect { public int Left, Top, Right, Bottom; }

    private static uint PreviewPixel(NativePreviewControl preview)
    {
        // Inspect this preview's rendered content. A desktop GetPixel can
        // inspect a different window when the user works during qualification.
        int width = (int)Math.Ceiling(preview.ActualWidth), height = (int)Math.Ceiling(preview.ActualHeight);
        var bitmap = new RenderTargetBitmap(width, height, 96, 96, PixelFormats.Pbgra32);
        bitmap.Render(preview);
        var pixel = new byte[4];
        bitmap.CopyPixels(new Int32Rect(width / 2, height / 2, 1, 1), pixel, 4, 0);
        return (uint)(pixel[2] | pixel[1] << 8 | pixel[0] << 16);
    }

    private static uint DesktopPixel(Point point)
    {
        nint dc = GetDC(0);
        try { return GetPixel(dc, (int)point.X, (int)point.Y); }
        finally { ReleaseDC(0, dc); }
    }

    private static async Task WorkstationAsync(App app)
    {
        var window = new RecordingScreenWindow { Topmost = true };
        window.Show();
        var vm = (RecordingScreenWindowVM)window.DataContext;
        await UntilAsync(() => vm.CanStartRecording, "Recorder did not load for monitor tests.");
        var primary = vm.ScreenOptions.First(option => option.Value?.Primary == true).Value!.Bounds;
        SetWindowPos(new WindowInteropHelper(window).Handle, 0, primary.Right - 770,
            primary.Bottom - (int)window.ActualHeight - 30, 740, (int)window.ActualHeight, 4);
        var preview = (NativePreviewControl)window.FindName("ScreenNativePreview");
        var applyRegion = typeof(RecordingScreenWindowVM).GetMethod("ApplyDrawArea", BindingFlags.NonPublic | BindingFlags.Instance)!;
        var monitors = vm.ScreenOptions.Where(option => option.Value is not null).ToArray();
        Check(monitors.Length >= 2, "This workstation validation requires two connected monitors.");
        foreach (var monitor in monitors)
        {
            var bounds = monitor.Value!.Bounds;
            var patch = new System.Windows.Forms.Panel { BackColor = System.Drawing.Color.Red,
                Left = 0, Top = 0, Width = 320, Height = 360 };
            using var target = new System.Windows.Forms.Form { FormBorderStyle = System.Windows.Forms.FormBorderStyle.None,
                ShowInTaskbar = false, TopMost = true, Width = 640, Height = 360,
                BackColor = System.Drawing.Color.Blue, StartPosition = System.Windows.Forms.FormStartPosition.Manual,
                Left = bounds.X + 48, Top = bounds.Y + 48, Text = "NessStudio capture validation" };
            target.Controls.Add(patch);
            target.Show();
            SetWindowPos(target.Handle, new nint(-1), bounds.X + 48, bounds.Y + 48, 640, 360, 0x50);
            try
            {
                foreach (var mode in Enum.GetValues<RecordingExecutionMode>().Where(mode => !_gpuOnlyPreviews || mode != RecordingExecutionMode.FullSoftware))
                {
                    vm.SelectedExecutionMode = mode;
                    vm.IsScreenEnabled = true;
                    vm.SelectedScreenOption = monitor;
                    long before = preview.PresentedFrames;
                    await UntilAsync(() => preview.Source is not null && preview.PresentedFrames > before + 3,
                        "Monitor preview stalled: " + monitor.Display + "/" + mode, () => preview.PresentationError);
                    var region = new Rect(bounds.X + 80, bounds.Y + 96, 256, 144);
                    patch.BackColor = System.Drawing.Color.Red;
                    patch.Refresh();
                    target.Activate();
                    SetWindowPos(target.Handle, new nint(-1), bounds.X + 48, bounds.Y + 48, 640, 360, 0x40);
                    applyRegion.Invoke(vm, [region]);
                    before = preview.PresentedFrames;
                    try
                    {
                        await UntilAsync(() => preview.PresentedFrames > before + 3 && (PreviewPixel(preview) & 0xff) > 150
                            && (PreviewPixel(preview) >> 16 & 0xff) < 90, "Draw Area preview did not show the selected red region: " + mode,
                            () => preview.PresentationError);
                    }
                    catch
                    {
                        Screenshot(window, "draw-area-failed-recorder");
                        using var inputImage = new System.Drawing.Bitmap(target.Width, target.Height);
                        target.DrawToBitmap(inputImage, new System.Drawing.Rectangle(0, 0, target.Width, target.Height));
                        inputImage.Save(Path.Combine(_output, "draw-area-failed-source.png"), System.Drawing.Imaging.ImageFormat.Png);
                        GetWindowRect(target.Handle, out var actualTarget);
                        GetWindowDisplayAffinity(target.Handle, out uint affinity);
                        DwmGetWindowAttribute(target.Handle, 9, out NativeRect physicalTarget, 16);
                        DwmGetWindowAttribute(target.Handle, 14, out uint cloaked, 4);
                        nint inputDc = GetDC(patch.Handle);
                        uint clientPixel;
                        try { clientPixel = GetPixel(inputDc, 80, 80); }
                        finally { ReleaseDC(patch.Handle, inputDc); }
                        nint sourceWindow = WindowFromPoint(new NativePoint { X = (int)(region.X + region.Width / 2), Y = (int)(region.Y + region.Height / 2) });
                        GetWindowThreadProcessId(sourceWindow, out uint sourceProcess);
                        File.WriteAllText(Path.Combine(_output, "draw-area-failed-pixels.json"), JsonSerializer.Serialize(new
                        {
                            mode, region = region.ToString(), renderedPreview = PreviewPixel(preview),
                            desktopPreview = DesktopPixel(preview.PointToScreen(new Point(preview.ActualWidth / 2, preview.ActualHeight / 2))),
                            desktopSource = DesktopPixel(new Point(region.X + region.Width / 2, region.Y + region.Height / 2)),
                            expectedProcessId = Environment.ProcessId, affinity, cloaked, clientPixel,
                            physicalBounds = new[] { physicalTarget.Left, physicalTarget.Top, physicalTarget.Right, physicalTarget.Bottom },
                            targetBounds = new[] { actualTarget.Left, actualTarget.Top, actualTarget.Right, actualTarget.Bottom },
                            targetHandle = target.Handle.ToInt64(), sourceWindow = sourceWindow.ToInt64(), sourceProcess,
                            preview.PresentedFrames, preview.PresentationError
                        }));
                        throw;
                    }
                    patch.BackColor = System.Drawing.Color.Lime;
                    patch.Refresh();
                    await UntilAsync(() => (PreviewPixel(preview) >> 8 & 0xff) > 150 && (PreviewPixel(preview) & 0xff) < 90,
                        "Draw Area preview did not update live: " + mode, () => preview.PresentationError);
                    Screenshot(window, "draw-area-" + Array.IndexOf(monitors, monitor) + "-" + mode);
                    Results.Add(new { name = "monitor-and-live-draw-area-preview", monitor = monitor.Display,
                        bounds, mode = mode.ToString(), crop = region.ToString(), changedFromRedToGreen = true });
                    File.WriteAllText(Path.Combine(_output, "progress.json"), JsonSerializer.Serialize(Results));
                }
            }
            finally { target.Close(); }
        }
        await window.PreviewWorkspace.DisposeAsync();
        window.Close();
    }
}

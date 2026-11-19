using System.Windows;
using System.Windows.Controls;
using System.Windows.Interop;
using System.Windows.Threading;
using NessStudio.Presentation;

internal static partial class Program
{
    [System.Runtime.InteropServices.DllImport("user32.dll")]
    private static extern bool IsWindowEnabled(nint window);

    private static void EnglishDialogs()
    {
        var owner = new Window { Title = "NessStudio dialog test owner", Width = 360, Height = 120 };
        owner.Show();
        foreach (var (buttons, labels) in new[]
        {
            (MessageBoxButton.OK, new[] { "OK" }),
            (MessageBoxButton.OKCancel, new[] { "OK", "Cancel" }),
            (MessageBoxButton.YesNo, new[] { "Yes", "No" }),
            (MessageBoxButton.YesNoCancel, new[] { "Yes", "No", "Cancel" })
        })
        foreach (string label in labels)
        {
            Exception? failure = null;
            var timer = new DispatcherTimer { Interval = TimeSpan.FromMilliseconds(150) };
            timer.Tick += (_, _) =>
            {
                timer.Stop();
                var dialog = Application.Current.Windows.OfType<Window>().Single(window => Equals(window.Tag, "EnglishMessageBox"));
                try
                {
                    var controls = Descendants<Button>(dialog).ToArray();
                    Check(controls.Select(button => (string)button.Content).SequenceEqual(labels), "Dialog buttons depend on Windows language.");
                    Check(dialog.Owner == owner && !IsWindowEnabled(new WindowInteropHelper(owner).Handle), "The dialog did not disable its owner.");
                    Check(controls.Single(button => button.IsDefault).Content.Equals(buttons == MessageBoxButton.YesNo ? "No" : labels[0]),
                        "The dialog lost its explicit default action.");
                    if (buttons == MessageBoxButton.YesNoCancel && label == "Cancel") Screenshot(dialog, "english-dialog-buttons");
                    controls.Single(button => button.Content.Equals(label)).RaiseEvent(new RoutedEventArgs(Button.ClickEvent));
                }
                catch (Exception error) { failure = error; dialog.Close(); }
            };
            timer.Start();
            var result = EnglishMessageBox.Show(owner, "Application messages and action buttons use English.", "Recording",
                buttons, MessageBoxImage.Warning, buttons == MessageBoxButton.YesNo ? MessageBoxResult.No : MessageBoxResult.None);
            if (failure is not null) throw failure;
            Check(result.ToString() == label && IsWindowEnabled(new WindowInteropHelper(owner).Handle), "Dialog result or owner restoration changed.");
            Results.Add(new { name = "english-modal-actions-and-owner", buttons = buttons.ToString(), selected = label });
        }
        var closeTimer = new DispatcherTimer { Interval = TimeSpan.FromMilliseconds(150) };
        Exception? longTextFailure = null;
        closeTimer.Tick += (_, _) =>
        {
            closeTimer.Stop();
            var dialog = Application.Current.Windows.OfType<Window>().Single(window => Equals(window.Tag, "EnglishMessageBox"));
            try
            {
                Check(Descendants<ScrollViewer>(dialog).Any(scroll => scroll.ScrollableHeight > 0), "Long diagnostics cannot be scrolled.");
                Check(dialog.ActualHeight < SystemParameters.WorkArea.Height, "Long diagnostics exceeded the desktop height.");
            }
            catch (Exception error) { longTextFailure = error; }
            dialog.Close();
        };
        closeTimer.Start();
        var cancelled = EnglishMessageBox.Show(owner, string.Join(Environment.NewLine, Enumerable.Repeat("A detailed diagnostic line.", 200)),
            "Recording", MessageBoxButton.OKCancel);
        if (longTextFailure is not null) throw longTextFailure;
        Check(cancelled == MessageBoxResult.Cancel, "Closing a confirmation accepted an action.");
        Results.Add(new { name = "long-diagnostics-scroll-and-close-cancels" });
        owner.Close();
    }
}

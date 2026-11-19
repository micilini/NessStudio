using System.Threading;
using System.Windows;
using System.Windows.Controls;
using System.Windows.Media;

namespace NessStudio.Presentation;

/// <summary>Application-owned modal messages whose text and buttons do not depend on Windows language resources.</summary>
public static class EnglishMessageBox
{
    public static MessageBoxResult Show(string text, string caption, MessageBoxButton buttons = MessageBoxButton.OK,
        MessageBoxImage image = MessageBoxImage.None, MessageBoxResult defaultResult = MessageBoxResult.None) =>
        Show(null, text, caption, buttons, image, defaultResult);

    public static MessageBoxResult Show(Window? owner, string text, string caption,
        MessageBoxButton buttons = MessageBoxButton.OK, MessageBoxImage image = MessageBoxImage.None,
        MessageBoxResult defaultResult = MessageBoxResult.None)
    {
        var dispatcher = owner?.Dispatcher ?? Application.Current?.Dispatcher;
        if (dispatcher is not null && !dispatcher.CheckAccess())
            return dispatcher.Invoke(() => Show(owner, text, caption, buttons, image, defaultResult));
        if (Thread.CurrentThread.GetApartmentState() != ApartmentState.STA)
        {
            MessageBoxResult result = MessageBoxResult.None;
            var thread = new Thread(() => result = Show(null, text, caption, buttons, image, defaultResult));
            thread.SetApartmentState(ApartmentState.STA);
            thread.Start(); thread.Join();
            return result;
        }

        MessageBoxResult[] choices = buttons switch
        {
            MessageBoxButton.OK => [MessageBoxResult.OK],
            MessageBoxButton.OKCancel => [MessageBoxResult.OK, MessageBoxResult.Cancel],
            MessageBoxButton.YesNo => [MessageBoxResult.Yes, MessageBoxResult.No],
            MessageBoxButton.YesNoCancel => [MessageBoxResult.Yes, MessageBoxResult.No, MessageBoxResult.Cancel],
            _ => throw new ArgumentOutOfRangeException(nameof(buttons))
        };
        var cancel = choices.Contains(MessageBoxResult.Cancel) ? MessageBoxResult.Cancel : choices[^1];
        var selected = cancel;
        if (!choices.Contains(defaultResult)) defaultResult = choices[0];
        owner ??= Application.Current?.Windows.OfType<Window>().FirstOrDefault(window => window.IsActive && window.IsVisible);
        var dialog = new Window
        {
            Title = caption, Tag = nameof(EnglishMessageBox), Width = 500,
            SizeToContent = SizeToContent.Height, ResizeMode = ResizeMode.NoResize,
            ShowInTaskbar = owner is null, WindowStartupLocation = owner is null
                ? WindowStartupLocation.CenterScreen : WindowStartupLocation.CenterOwner,
            Background = new SolidColorBrush(Color.FromRgb(32, 36, 39)),
            Foreground = new SolidColorBrush(Color.FromRgb(204, 213, 215)),
            FontFamily = new FontFamily("Segoe UI"), FontSize = 14
        };
        if (owner is { IsLoaded: true }) dialog.Owner = owner;
        var body = new DockPanel { Margin = new Thickness(22) };
        var actions = new StackPanel { Orientation = Orientation.Horizontal,
            HorizontalAlignment = HorizontalAlignment.Right, Margin = new Thickness(0, 20, 0, 0) };
        DockPanel.SetDock(actions, Dock.Bottom);
        body.Children.Add(actions);
        foreach (var choice in choices)
        {
            var button = new Button
            {
                Content = choice.ToString(), MinWidth = 84, Padding = new Thickness(14, 7, 14, 7),
                Margin = new Thickness(8, 0, 0, 0), IsDefault = choice == defaultResult,
                IsCancel = choice == cancel, Style = null, Foreground = Brushes.Black,
                Background = SystemColors.ControlBrush
            };
            button.Click += (_, _) => { selected = choice; dialog.Close(); };
            actions.Children.Add(button);
        }
        var message = new DockPanel();
        if (image != MessageBoxImage.None)
        {
            var icon = new TextBlock { Text = image == MessageBoxImage.Question ? "?" :
                image == MessageBoxImage.Information ? "i" : "!", FontSize = 28, FontWeight = FontWeights.Bold,
                Width = 28, Margin = new Thickness(0, 0, 14, 0),
                Foreground = new SolidColorBrush(image == MessageBoxImage.Error
                    ? Color.FromRgb(231, 76, 60) : Color.FromRgb(83, 132, 181)) };
            DockPanel.SetDock(icon, Dock.Left);
            message.Children.Add(icon);
        }
        message.Children.Add(new ScrollViewer { VerticalScrollBarVisibility = ScrollBarVisibility.Auto,
            MaxHeight = Math.Max(100, SystemParameters.WorkArea.Height * 0.6),
            Content = new TextBlock { Text = text, TextWrapping = TextWrapping.Wrap } });
        body.Children.Add(message);
        dialog.Content = body;
        dialog.Loaded += (_, _) => actions.Children.OfType<Button>().First(button => button.IsDefault).Focus();
        dialog.ShowDialog();
        return selected;
    }
}

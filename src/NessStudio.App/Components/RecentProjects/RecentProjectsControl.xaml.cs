using MaterialDesignThemes.Wpf;
using NessStudio.Models;
using NessStudio.Recording;
using NessStudio.View.ExportScreen;
using NessStudio.ViewModel;
using NessStudio.ViewModel.Helpers;
using System;
using System.Collections.Generic;
using System.IO;
using System.Threading.Tasks;
using System.Windows;
using System.Windows.Controls;
using System.Windows.Controls.Primitives;
using System.Windows.Data;
using System.Windows.Input;
using System.Windows.Media;
using System.Windows.Media.Imaging;

namespace NessStudio.Components.RecentProjects
{
    public partial class RecentProjectsControl : UserControl
    {
        public HomeScreenWindowVM HomeScreenWindowVM { get; set; }
        public RecentProjectsControlVM RecentProjectsControlVM { get; set; }

        public RecentProjectsControl(HomeScreenWindowVM HS)
        {
            InitializeComponent();
            HomeScreenWindowVM = HS;
            RecentProjectsControlVM = new RecentProjectsControlVM(HS, this);
            this.DataContext = RecentProjectsControlVM;
        }

        public void RenderProjects(IEnumerable<ProjectsModel> items, bool clearFirst = true)
        {
            if (!Dispatcher.CheckAccess())
            {
                Dispatcher.Invoke(() => RenderProjects(items, clearFirst));
                return;
            }

            if (clearFirst)
                ProjectsWrapPanel.Children.Clear();

            foreach (var model in items)
                ProjectsWrapPanel.Children.Add(BuildProjectCard(model));
        }

        private UIElement BuildProjectCard(ProjectsModel model, bool deferImage = false)
        {
            Brush borderBrush = (Brush)new BrushConverter().ConvertFromString("#2A2F33");
            Brush cardBg = (Brush)new BrushConverter().ConvertFromString("#232629");
            Brush thumbBg = (Brush)new BrushConverter().ConvertFromString("#1C1F21");
            Brush fg = (Brush)new BrushConverter().ConvertFromString("#708089");

            const int cardWidth = 225;
            const int cardHeight = 223;
            const int cardPadding = 9;
            const int thumbHeight = 112;
            const int cardMargin = 6;

            var card = new Border
            {
                CornerRadius = new CornerRadius(8),
                BorderBrush = borderBrush,
                BorderThickness = new Thickness(1),
                Background = cardBg,
                Padding = new Thickness(cardPadding),
                Width = cardWidth,
                Height = cardHeight,
                Margin = new Thickness(cardMargin),
                Tag = model.ProjectFolderPath
            };

            var root = new Grid();
            root.RowDefinitions.Add(new RowDefinition { Height = new GridLength(thumbHeight) });
            root.RowDefinitions.Add(new RowDefinition { Height = new GridLength(40) });
            root.RowDefinitions.Add(new RowDefinition { Height = new GridLength(8) });
            root.RowDefinitions.Add(new RowDefinition { Height = new GridLength(20) });
            root.RowDefinitions.Add(new RowDefinition { Height = new GridLength(1, GridUnitType.Star) });

            var thumbBorder = new Border
            {
                Background = thumbBg,
                Height = thumbHeight,
                VerticalAlignment = VerticalAlignment.Top,
                CornerRadius = new CornerRadius(8),
                ClipToBounds = true
            };

            var thumbButton = new Button
            {
                Background = Brushes.Transparent,
                BorderBrush = Brushes.Transparent,
                BorderThickness = new Thickness(0),
                Padding = new Thickness(0),
                Cursor = Cursors.Hand,
                HorizontalAlignment = HorizontalAlignment.Stretch,
                VerticalAlignment = VerticalAlignment.Stretch,
                HorizontalContentAlignment = HorizontalAlignment.Stretch,
                VerticalContentAlignment = VerticalAlignment.Stretch
            };

            thumbButton.Click += async (s, e) => await RunProjectActionAsync(model, card,
                () => RecentProjectsControlVM.OpenInExplorer(model.ProjectFolderPath));

            BindingOperations.SetBinding(thumbButton, FrameworkElement.HeightProperty,
                new Binding("ActualHeight") { Source = thumbBorder });
            BindingOperations.SetBinding(thumbButton, FrameworkElement.WidthProperty,
                new Binding("ActualWidth") { Source = thumbBorder });

            var img = new Image
            {
                Stretch = Stretch.UniformToFill,
                HorizontalAlignment = HorizontalAlignment.Stretch,
                VerticalAlignment = VerticalAlignment.Stretch
            };

            BindingOperations.SetBinding(img, FrameworkElement.HeightProperty,
                new Binding("ActualHeight") { Source = thumbBorder });
            BindingOperations.SetBinding(img, FrameworkElement.WidthProperty,
                new Binding("ActualWidth") { Source = thumbBorder });

            const string FallbackPackUri = "pack://application:,,,/Assets/Images/system-audio-icon.png";

            try
            {
                if (!string.IsNullOrWhiteSpace(model.ThumbnailPath) && File.Exists(model.ThumbnailPath))
                {
                    img.Source = LoadBitmapHelper.LoadBitmapFromFile(model.ThumbnailPath);
                    img.Stretch = Stretch.UniformToFill;
                    img.Width = Double.NaN;
                    img.Height = Double.NaN;
                }
                else
                {
                    img.Source = LoadBitmapHelper.LoadBitmapFromPack(FallbackPackUri);
                    img.Stretch = Stretch.Uniform;
                    img.Width = 64;
                    img.Height = 64;
                    img.HorizontalAlignment = HorizontalAlignment.Center;
                    img.VerticalAlignment = VerticalAlignment.Center;
                }
            }
            catch
            {
                try { img.Source = LoadBitmapHelper.LoadBitmapFromPack(FallbackPackUri); } catch { }
            }

            thumbButton.Content = img;
            thumbBorder.Child = thumbButton;

            if (!string.IsNullOrWhiteSpace(model.ThumbnailPath) && File.Exists(model.ThumbnailPath))
            {
                _ = Task.Run(() =>
                {
                    try
                    {
                        using (var fs = File.OpenRead(model.ThumbnailPath))
                        {
                            var ms = new MemoryStream();
                            fs.CopyTo(ms);
                            ms.Position = 0;

                            var bmp = new BitmapImage();
                            bmp.BeginInit();
                            bmp.CacheOption = BitmapCacheOption.OnLoad;
                            bmp.CreateOptions = BitmapCreateOptions.IgnoreColorProfile;
                            bmp.StreamSource = ms;
                            bmp.EndInit();
                            bmp.Freeze();

                            return (ImageSource)bmp;
                        }
                    }
                    catch
                    {
                        try
                        {
                            var fb = LoadBitmapHelper.LoadBitmapFromPack(FallbackPackUri);
                            fb?.Freeze();
                            return (ImageSource)fb;
                        }
                        catch { return null; }
                    }
                }).ContinueWith(t =>
                {
                    if (t.Status == TaskStatus.RanToCompletion && t.Result != null)
                    {
                        Dispatcher.Invoke(() =>
                        {
                            img.Stretch = Stretch.UniformToFill;
                            img.Width = Double.NaN;
                            img.Height = Double.NaN;
                            img.Source = t.Result;
                        });
                    }
                }, TaskScheduler.FromCurrentSynchronizationContext());
            }

            string title = model.DisplayTitle;
            var titleText = new TextBlock
            {
                Text = title,
                ToolTip = title,
                FontSize = 15,
                FontFamily = new FontFamily("Arial"),
                FontWeight = FontWeights.Bold,
                Foreground = fg,
                VerticalAlignment = VerticalAlignment.Center,
                MaxHeight = 36,
                LineHeight = 18,
                LineStackingStrategy = LineStackingStrategy.BlockLineHeight,
                TextWrapping = TextWrapping.Wrap,
                TextTrimming = TextTrimming.CharacterEllipsis
            };

            var sizeText = new TextBlock
            {
                Text = model.FileSizeBytes > 0 ? model.FileSizeHuman : "—",
                FontSize = 14,
                FontFamily = new FontFamily("Arial"),
                FontWeight = FontWeights.Medium,
                Foreground = fg,
                VerticalAlignment = VerticalAlignment.Center
            };

            var dateText = new TextBlock
            {
                VerticalAlignment = VerticalAlignment.Center,
                Text = model.UpdatedAt.ToString("MM/dd/yyyy HH:mm", System.Globalization.CultureInfo.InvariantCulture),
                FontSize = 12,
                FontFamily = new FontFamily("Arial"),
                FontWeight = FontWeights.Medium,
                Foreground = fg
            };

            var titleGrid = new Grid { Margin = new Thickness(0, 4, 0, 0) };
            titleGrid.ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(1, GridUnitType.Star) });
            titleGrid.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });
            titleGrid.Children.Add(titleText);

            var optionsBtn = new Button
            {
                Style = (Style)FindResource("RecordingOptionsButtonStyle"),
                Width = 32,
                Height = 32,
                VerticalAlignment = VerticalAlignment.Center,
                Padding = new Thickness(0),
                Margin = new Thickness(8, 0, 0, 0),
                Cursor = Cursors.Hand,
                Tag = model,
                ToolTip = "More options",
                Content = new PackIcon
                {
                    Kind = PackIconKind.DotsVertical,
                    Width = 24,
                    Height = 24,
                    HorizontalAlignment = HorizontalAlignment.Center,
                    VerticalAlignment = VerticalAlignment.Center
                }
            };
            System.Windows.Automation.AutomationProperties.SetName(optionsBtn, "More options");

            optionsBtn.Click += (s, e) => OpenOptionsMenu(optionsBtn, model, card);

            Grid.SetColumn(optionsBtn, 1);
            titleGrid.Children.Add(optionsBtn);

            Grid.SetRow(titleGrid, 1);
            Grid.SetRow(sizeText, 3);
            Grid.SetRow(dateText, 4);
            root.Children.Add(thumbBorder);
            root.Children.Add(titleGrid);
            root.Children.Add(sizeText);
            root.Children.Add(dateText);

            card.Child = root;
            return card;
        }

        private void OpenOptionsMenu(Button ownerButton, ProjectsModel model, Border card)
        {
            var menu = new ContextMenu
            {
                Background = BrushFrom("#191D20"),
                BorderBrush = BrushFrom("#31383D"),
                BorderThickness = new Thickness(1),
                Padding = new Thickness(8),
                PlacementTarget = ownerButton,
                Placement = PlacementMode.Bottom,
                StaysOpen = false,
                MinWidth = 158
            };

            var openItem = BuildMenuItem("📂", "Open", false);
            openItem.Click += async (s, e) => await RunProjectActionAsync(model, card,
                () => RecentProjectsControlVM.OpenInExplorer(model.ProjectFolderPath));

            var exportItem = BuildMenuItem("↓", "Export", false);
            exportItem.Click += async (s, e) => await RunProjectActionAsync(model, card, () => OpenExportWindow(model));

            var deleteItem = BuildMenuItem("🗑", "Delete", true);
            deleteItem.Click += async (s, e) => await DeleteProjectFromCardAsync(model, card);

            menu.Items.Add(openItem);
            menu.Items.Add(exportItem);
            menu.Items.Add(new Separator { Background = BrushFrom("#31383D"), Margin = new Thickness(4, 6, 4, 6) });
            menu.Items.Add(deleteItem);

            ownerButton.ContextMenu = menu;
            menu.IsOpen = true;
        }

        private MenuItem BuildMenuItem(string icon, string text, bool danger)
        {
            var panel = new StackPanel { Orientation = Orientation.Horizontal };
            panel.Children.Add(new TextBlock
            {
                Text = icon,
                Width = 24,
                Foreground = danger ? BrushFrom("#FF9E9E") : BrushFrom("#CCD5D7"),
                FontFamily = new FontFamily("Arial"),
                FontSize = 14,
                VerticalAlignment = VerticalAlignment.Center
            });
            panel.Children.Add(new TextBlock
            {
                Text = text,
                Foreground = danger ? BrushFrom("#FF9E9E") : BrushFrom("#CCD5D7"),
                FontFamily = new FontFamily("Arial"),
                FontSize = 14,
                VerticalAlignment = VerticalAlignment.Center
            });

            return new MenuItem
            {
                Header = panel,
                Padding = new Thickness(10, 8, 10, 8),
                Background = Brushes.Transparent,
                BorderBrush = Brushes.Transparent,
                Cursor = Cursors.Hand
            };
        }

        private void OpenExportWindow(ProjectsModel model)
        {
            try
            {
                var owner = Window.GetWindow(this);
                var exportWindow = new ExportScreenWindow(model)
                {
                    Owner = owner,
                    WindowStartupLocation = WindowStartupLocation.CenterOwner
                };

                exportWindow.ShowDialog();
            }
            catch (Exception ex)
            {
                DebugLog.Write("[RecentProjects] OpenExportWindow ERROR:\n" + UserError.Details(ex));
                MessageBox.Show(
                    "Unable to open the export window.\n\n" + UserError.Message(ex),
                    "Export Recording",
                    MessageBoxButton.OK,
                    MessageBoxImage.Error);
            }
        }

        private readonly HashSet<int> _pendingProjectActions = new();

        private async Task RunProjectActionAsync(ProjectsModel model, Border card, Action action)
        {
            if (!_pendingProjectActions.Add(model.Id)) return;
            try
            {
                var validation = await Task.Run(() => ProjectFilesValidation.Check(model.ProjectFolderPath));
                if (validation.IsValid)
                {
                    action();
                    return;
                }

                var result = MessageBox.Show(Window.GetWindow(this), validation.Message, "Recording Files",
                    MessageBoxButton.OK, MessageBoxImage.Error);
                if (result == MessageBoxResult.OK && validation.RemoveFromLibrary)
                {
                    if (RecentProjectsControlVM.RemoveProjectFromLibrary(model))
                        RemoveProjectCard(card);
                    else
                        MessageBox.Show("Failed to remove this recording from the library. Please try again.",
                            "Recording Files", MessageBoxButton.OK, MessageBoxImage.Error);
                }
            }
            catch (Exception error)
            {
                DebugLog.Write("[RecentProjects] Project action ERROR:\n" + UserError.Details(error));
                MessageBox.Show("Unable to open this recording.\n\n" + UserError.Message(error),
                    "Recording Files", MessageBoxButton.OK, MessageBoxImage.Error);
            }
            finally { _pendingProjectActions.Remove(model.Id); }
        }

        private void RemoveProjectCard(Border card)
        {
            if (ProjectsWrapPanel.Children.Contains(card))
                ProjectsWrapPanel.Children.Remove(card);

            if (ProjectsWrapPanel.Children.Count == 0)
            {
                RecentProjectsControlVM.IsNoneProjectBlockVisible = true;
                RecentProjectsControlVM.IsProjectBlockVisible = false;
            }
        }

        public async Task RenderProjectsAsync(IEnumerable<ProjectsModel> items, bool clearFirst = true)
        {
            if (!Dispatcher.CheckAccess())
            {
                await Dispatcher.InvokeAsync(() => RenderProjectsAsync(items, clearFirst));
                return;
            }

            if (clearFirst)
                ProjectsWrapPanel.Children.Clear();

            var sw = System.Diagnostics.Stopwatch.StartNew();
            int batchMs = 16;

            foreach (var model in items)
            {
                var card = BuildProjectCard(model, deferImage: true);
                ProjectsWrapPanel.Children.Add(card);

                if (sw.ElapsedMilliseconds > batchMs)
                {
                    sw.Restart();
                    await Task.Yield();
                }
            }
        }

        private async Task DeleteProjectFromCardAsync(ProjectsModel model, Border card)
        {
            var result = MessageBox.Show(
                "Do you really want to delete this recording?\nThis action cannot be undone.",
                "Delete Recording",
                MessageBoxButton.YesNo,
                MessageBoxImage.Warning);

            if (result != MessageBoxResult.Yes)
                return;

            bool ok = await RecentProjectsControlVM.DeleteProjectAsync(model);

            if (ok)
            {
                RemoveProjectCard(card);
            }
            else
            {
                MessageBox.Show("Failed to delete this recording. Please try again.",
                    "Delete Recording", MessageBoxButton.OK, MessageBoxImage.Error);
            }
        }

        private static Brush BrushFrom(string hex)
        {
            return (Brush)new BrushConverter().ConvertFromString(hex)!;
        }
    }
}

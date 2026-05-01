using MaterialDesignThemes.Wpf;
using NessStudio.Models;
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
            const int cardHeight = 215;
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

            var root = new StackPanel();

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

            thumbButton.Click += (s, e) => RecentProjectsControlVM.OpenInExplorer(model.ProjectFolderPath);

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

            var titleBox = new TextBox
            {
                Margin = new Thickness(0, 8, 0, 0),
                Text = string.IsNullOrWhiteSpace(model.Title) ? "Unknown Title" : model.Title,
                FontSize = 15,
                FontFamily = new FontFamily("Arial"),
                FontWeight = FontWeights.Bold,
                Foreground = fg,
                Background = Brushes.Transparent,
                BorderThickness = new Thickness(0),
                Padding = new Thickness(0),
                IsReadOnly = true,
                IsTabStop = false,
                Cursor = Cursors.IBeam,
                TextWrapping = TextWrapping.Wrap
            };

            var sizeBox = new TextBox
            {
                Margin = new Thickness(0, 8, 0, 0),
                Text = model.FileSizeBytes > 0 ? model.FileSizeHuman : "—",
                FontSize = 14,
                FontFamily = new FontFamily("Arial"),
                FontWeight = FontWeights.Medium,
                Foreground = fg,
                Background = Brushes.Transparent,
                BorderThickness = new Thickness(0),
                Padding = new Thickness(0),
                IsReadOnly = true,
                IsTabStop = false,
                Cursor = Cursors.IBeam,
                TextWrapping = TextWrapping.Wrap
            };

            var dateBox = new TextBox
            {
                Margin = new Thickness(0, 8, 0, 0),
                Text = model.UpdatedAt.ToString("dd/MM/yyyy HH:mm"),
                FontSize = 12,
                FontFamily = new FontFamily("Arial"),
                FontWeight = FontWeights.Medium,
                Foreground = fg,
                Background = Brushes.Transparent,
                BorderThickness = new Thickness(0),
                Padding = new Thickness(0),
                IsReadOnly = true,
                IsTabStop = false,
                Cursor = Cursors.IBeam,
                TextWrapping = TextWrapping.Wrap,
                TextAlignment = TextAlignment.Left
            };

            TextFieldAssist.SetDecorationVisibility(titleBox, Visibility.Collapsed);
            TextFieldAssist.SetUnderlineBrush(titleBox, Brushes.Transparent);
            TextFieldAssist.SetDecorationVisibility(sizeBox, Visibility.Collapsed);
            TextFieldAssist.SetUnderlineBrush(sizeBox, Brushes.Transparent);
            TextFieldAssist.SetDecorationVisibility(dateBox, Visibility.Collapsed);
            TextFieldAssist.SetUnderlineBrush(dateBox, Brushes.Transparent);

            titleBox.FocusVisualStyle = null;
            sizeBox.FocusVisualStyle = null;
            dateBox.FocusVisualStyle = null;

            var bottomGrid = new Grid { Margin = new Thickness(0, 8, 0, 0) };
            bottomGrid.ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(1, GridUnitType.Star) });
            bottomGrid.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });

            Grid.SetColumn(dateBox, 0);
            bottomGrid.Children.Add(dateBox);

            var optionsBtn = new Button
            {
                Width = 28,
                Height = 28,
                Padding = new Thickness(0),
                Margin = new Thickness(8, 0, 0, 0),
                Background = Brushes.Transparent,
                BorderBrush = Brushes.Transparent,
                BorderThickness = new Thickness(0),
                Cursor = Cursors.Hand,
                Tag = model,
                ToolTip = "More options",
                Content = new TextBlock
                {
                    Text = "⋮",
                    Foreground = fg,
                    FontSize = 24,
                    FontFamily = new FontFamily("Arial"),
                    FontWeight = FontWeights.Bold,
                    HorizontalAlignment = HorizontalAlignment.Center,
                    VerticalAlignment = VerticalAlignment.Center,
                    Margin = new Thickness(0, -4, 0, 0)
                }
            };

            optionsBtn.Click += (s, e) => OpenOptionsMenu(optionsBtn, model, card);

            Grid.SetColumn(optionsBtn, 1);
            bottomGrid.Children.Add(optionsBtn);

            root.Children.Add(thumbBorder);
            root.Children.Add(titleBox);
            root.Children.Add(sizeBox);
            root.Children.Add(bottomGrid);

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
            openItem.Click += (s, e) => RecentProjectsControlVM.OpenInExplorer(model.ProjectFolderPath);

            var exportItem = BuildMenuItem("↓", "Export", false);
            exportItem.Click += (s, e) => OpenExportWindow(model);

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
                if (string.IsNullOrWhiteSpace(model.ProjectFolderPath) || !Directory.Exists(model.ProjectFolderPath))
                {
                    MessageBox.Show(
                        "The recording folder was not found.",
                        "Export Recording",
                        MessageBoxButton.OK,
                        MessageBoxImage.Warning);
                    return;
                }

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
                DebugLog.Write("[RecentProjects] OpenExportWindow ERROR:\n" + ex);
                MessageBox.Show(
                    "Unable to open the export window.\n\n" + ex.Message,
                    "Export Recording",
                    MessageBoxButton.OK,
                    MessageBoxImage.Error);
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
                if (ProjectsWrapPanel.Children.Contains(card))
                    ProjectsWrapPanel.Children.Remove(card);

                if (ProjectsWrapPanel.Children.Count == 0)
                {
                    RecentProjectsControlVM.IsNoneProjectBlockVisible = true;
                    RecentProjectsControlVM.IsProjectBlockVisible = false;
                    ProjectsWrapPanel.Visibility = Visibility.Collapsed;
                }
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

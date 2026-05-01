using NessStudio.Models;
using NessStudio.ViewModel.Helpers;
using Microsoft.Win32;
using System;
using System.Diagnostics;
using System.IO;
using System.Linq;
using System.Threading;
using System.Threading.Tasks;
using System.Windows;
using System.Windows.Controls;
using System.Windows.Input;
using System.Windows.Media;
using System.Windows.Media.Imaging;

namespace NessStudio.View.ExportScreen
{
    public partial class ExportScreenWindow : Window
    {
        private readonly ProjectsModel _project;
        private RecordingExportSessionInfo? _sessionInfo;
        private RecordingExportMode _mode = RecordingExportMode.SingleFile;
        private CancellationTokenSource? _exportCancellation;
        private bool _isExporting;
        private bool _isInitializing = true;

        public ExportScreenWindow(ProjectsModel project)
        {
            _project = project ?? throw new ArgumentNullException(nameof(project));
            InitializeComponent();
            Loaded += ExportScreenWindow_Loaded;
        }

        private void ExportScreenWindow_Loaded(object sender, RoutedEventArgs e)
        {
            try
            {
                ProjectTitleText.Text = _project.Title;
                _sessionInfo = ExportService.LoadSessionInfo(_project.ProjectFolderPath);

                ConfigureInitialOptions();
                RenderDetectedTracks();
                SetExportMode(RecordingExportMode.SingleFile);
                UpdateOutputPathForMode();
                UpdateSummary();
            }
            catch (Exception ex)
            {
                ExportStatusTextBlock.Text = "Unable to load recording export data.";
                MessageBox.Show(
                    "Unable to load recording export data.\n\n" + ex.Message,
                    "Export Recording",
                    MessageBoxButton.OK,
                    MessageBoxImage.Error);
                Close();
            }
            finally
            {
                _isInitializing = false;
            }
        }

        private void ConfigureInitialOptions()
        {
            if (_sessionInfo == null)
                return;

            SetComboBoxItemEnabled(VideoLayoutComboBox, "NoVideo", _sessionInfo.HasAnyAudio);
            SetComboBoxItemEnabled(VideoLayoutComboBox, "ScreenOnly", _sessionInfo.HasScreen);
            SetComboBoxItemEnabled(VideoLayoutComboBox, "ScreenWebcamPictureInPicture", _sessionInfo.HasScreen && _sessionInfo.HasWebcam);
            SetComboBoxItemEnabled(VideoLayoutComboBox, "ScreenWebcamSideBySide", _sessionInfo.HasScreen && _sessionInfo.HasWebcam);
            SetComboBoxItemEnabled(VideoLayoutComboBox, "WebcamOnly", _sessionInfo.HasWebcam);

            SetComboBoxItemEnabled(AudioMixComboBox, "MicrophoneOnly", _sessionInfo.HasMic);
            SetComboBoxItemEnabled(AudioMixComboBox, "SystemAudioOnly", _sessionInfo.HasSystemAudio);
            SetComboBoxItemEnabled(AudioMixComboBox, "MicrophoneAndSystemAudio", _sessionInfo.HasMic && _sessionInfo.HasSystemAudio);

            if (_sessionInfo.HasScreen && _sessionInfo.HasWebcam)
                SelectComboBoxTag(VideoLayoutComboBox, "ScreenWebcamPictureInPicture");
            else if (_sessionInfo.HasScreen)
                SelectComboBoxTag(VideoLayoutComboBox, "ScreenOnly");
            else if (_sessionInfo.HasWebcam)
                SelectComboBoxTag(VideoLayoutComboBox, "WebcamOnly");
            else if (_sessionInfo.HasAnyAudio)
                SelectComboBoxTag(VideoLayoutComboBox, "NoVideo");
            else
                VideoLayoutComboBox.SelectedIndex = -1;

            if (_sessionInfo.HasMic && _sessionInfo.HasSystemAudio)
                SelectComboBoxTag(AudioMixComboBox, "MicrophoneAndSystemAudio");
            else if (_sessionInfo.HasMic)
                SelectComboBoxTag(AudioMixComboBox, "MicrophoneOnly");
            else if (_sessionInfo.HasSystemAudio)
                SelectComboBoxTag(AudioMixComboBox, "SystemAudioOnly");
            else
                SelectComboBoxTag(AudioMixComboBox, "NoAudio");

            SelectComboBoxTag(ContainerComboBox, "Mp4");
            UpdateContainerOptionsForCurrentLayout(forceAudioDefault: GetSelectedVideoLayout() == RecordingExportVideoLayout.NoVideo);
            UpdateQualityOptionsForCurrentContainer(forceDefault: true);

            ExportScreenCheckBox.IsChecked = _sessionInfo.HasScreen;
            ExportScreenCheckBox.IsEnabled = _sessionInfo.HasScreen;

            ExportWebcamCheckBox.IsChecked = _sessionInfo.HasWebcam;
            ExportWebcamCheckBox.IsEnabled = _sessionInfo.HasWebcam;

            ExportAudioMixCheckBox.IsChecked = _sessionInfo.HasAnyAudio;
            ExportAudioMixCheckBox.IsEnabled = _sessionInfo.HasAnyAudio;
        }

        private void RenderDetectedTracks()
        {
            if (_sessionInfo == null)
                return;

            TrackListPanel.Children.Clear();
            TrackListPanel.Children.Add(BuildTrackRow("Assets/Images/screen-icon.png", "Screen", "screen.mkv", _sessionInfo.HasScreen));
            TrackListPanel.Children.Add(BuildTrackRow("Assets/Images/webcam-icon.png", "Webcam", "webcam.mp4", _sessionInfo.HasWebcam));
            TrackListPanel.Children.Add(BuildTrackRow("Assets/Images/microphone-icon.png", "Microphone", "mic.wav", _sessionInfo.HasMic));
            TrackListPanel.Children.Add(BuildTrackRow("Assets/Images/systemaudio-icon.png", "System Audio", "system.wav", _sessionInfo.HasSystemAudio));
        }

        private UIElement BuildTrackRow(string iconPath, string title, string fileName, bool ready)
        {
            var row = new Border
            {
                Background = BrushFrom("#202427"),
                BorderBrush = BrushFrom("#343B40"),
                BorderThickness = new Thickness(1),
                CornerRadius = new CornerRadius(12),
                Padding = new Thickness(11, 10, 11, 10),
                Margin = new Thickness(0, 0, 0, 9)
            };

            var grid = new Grid();
            grid.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });
            grid.ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(1, GridUnitType.Star) });
            grid.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });

            var iconImage = new Image
            {
                Source = LoadAssetImage(iconPath),
                Width = 20,
                Height = 20,
                Stretch = Stretch.Uniform,
                HorizontalAlignment = HorizontalAlignment.Center,
                VerticalAlignment = VerticalAlignment.Center
            };

            RenderOptions.SetBitmapScalingMode(iconImage, BitmapScalingMode.HighQuality);

            var iconBox = new Border
            {
                Width = 30,
                Height = 30,
                CornerRadius = new CornerRadius(10),
                Background = BrushFrom("#2C3237"),
                Margin = new Thickness(0, 0, 10, 0),
                Child = iconImage
            };

            var textStack = new StackPanel { VerticalAlignment = VerticalAlignment.Center };
            textStack.Children.Add(new TextBlock
            {
                Text = title,
                Foreground = BrushFrom("#CCD5D7"),
                FontFamily = new FontFamily("Arial"),
                FontWeight = FontWeights.Bold,
                FontSize = 13
            });
            textStack.Children.Add(new TextBlock
            {
                Text = fileName,
                Foreground = BrushFrom("#708089"),
                FontFamily = new FontFamily("Arial"),
                FontSize = 12,
                Margin = new Thickness(0, 3, 0, 0)
            });

            var status = new TextBlock
            {
                Text = ready ? "Ready" : "Missing",
                Foreground = ready ? BrushFrom("#A8F5D6") : BrushFrom("#FFB1B1"),
                FontFamily = new FontFamily("Arial"),
                FontSize = 12,
                FontWeight = FontWeights.SemiBold,
                VerticalAlignment = VerticalAlignment.Center,
                Margin = new Thickness(12, 0, 0, 0)
            };

            Grid.SetColumn(iconBox, 0);
            Grid.SetColumn(textStack, 1);
            Grid.SetColumn(status, 2);

            grid.Children.Add(iconBox);
            grid.Children.Add(textStack);
            grid.Children.Add(status);
            row.Child = grid;

            return row;
        }

        private void SingleModeCard_MouseLeftButtonUp(object sender, MouseButtonEventArgs e)
        {
            SetExportMode(RecordingExportMode.SingleFile);
        }

        private void TracksModeCard_MouseLeftButtonUp(object sender, MouseButtonEventArgs e)
        {
            SetExportMode(RecordingExportMode.SeparateTracks);
        }

        private void SetExportMode(RecordingExportMode mode)
        {
            _mode = mode;

            bool isSingle = _mode == RecordingExportMode.SingleFile;

            SingleModeCard.Background = BrushFrom(isSingle ? "#2A2022" : "#202427");
            SingleModeCard.BorderBrush = BrushFrom(isSingle ? "#B33333" : "#343B40");

            TracksModeCard.Background = BrushFrom(!isSingle ? "#2A2022" : "#202427");
            TracksModeCard.BorderBrush = BrushFrom(!isSingle ? "#B33333" : "#343B40");

            SingleOptionsPanel.Visibility = isSingle ? Visibility.Visible : Visibility.Collapsed;
            TrackOptionsPanel.Visibility = isSingle ? Visibility.Collapsed : Visibility.Visible;

            UpdateOutputPathForMode();
            UpdateSummary();
        }

        private void ExportOption_Changed(object sender, RoutedEventArgs e)
        {
            if (_isInitializing)
                return;

            if (sender == VideoLayoutComboBox)
            {
                UpdateContainerOptionsForCurrentLayout(forceAudioDefault: GetSelectedVideoLayout() == RecordingExportVideoLayout.NoVideo);
                UpdateQualityOptionsForCurrentContainer(forceDefault: true);
                UpdateOutputPathForMode();
            }
            else if (sender == ContainerComboBox)
            {
                UpdateQualityOptionsForCurrentContainer(forceDefault: true);
                UpdateOutputPathForMode();
            }
            else if (sender == QualityComboBox)
            {
                UpdateOutputPathForMode();
            }

            UpdateSummary();
        }

        private void BrowseButton_Click(object sender, RoutedEventArgs e)
        {
            if (_sessionInfo == null)
                return;

            if (_mode == RecordingExportMode.SingleFile)
            {
                var container = GetSelectedContainer();
                var ext = GetContainerExtension(container);
                var dialog = new SaveFileDialog
                {
                    Title = "Export Recording",
                    FileName = Path.GetFileName(OutputPathTextBox.Text),
                    InitialDirectory = ResolveInitialDirectory(OutputPathTextBox.Text),
                    Filter = BuildSingleFileDialogFilter(container),
                    DefaultExt = ext,
                    AddExtension = true,
                    OverwritePrompt = true
                };

                if (dialog.ShowDialog(this) == true)
                    OutputPathTextBox.Text = dialog.FileName;
            }
            else
            {
                var initialDirectory = Directory.Exists(OutputPathTextBox.Text)
                    ? OutputPathTextBox.Text
                    : ExportService.BuildDefaultSeparateTracksOutputFolder(_sessionInfo.ProjectFolder);

                var dialog = new OpenFolderDialog
                {
                    Title = "Select the folder where NessStudio should export the separated tracks.",
                    InitialDirectory = initialDirectory,
                    Multiselect = false
                };

                if (dialog.ShowDialog(this) == true)
                    OutputPathTextBox.Text = dialog.FolderName;
            }
        }

        private async void StartExportButton_Click(object sender, RoutedEventArgs e)
        {
            if (_sessionInfo == null)
                return;

            if (_isExporting)
                return;

            var request = BuildRequest();
            var validation = ValidateRequest(request);

            if (!string.IsNullOrWhiteSpace(validation))
            {
                MessageBox.Show(validation, "Export Recording", MessageBoxButton.OK, MessageBoxImage.Warning);
                return;
            }

            SetExportingState(true);
            _exportCancellation = new CancellationTokenSource();

            try
            {
                var progress = new Progress<string>(message =>
                {
                    if (!string.IsNullOrWhiteSpace(message))
                        ExportStatusTextBlock.Text = message;
                });

                var result = await ExportService.ExportAsync(request, progress, _exportCancellation.Token);

                ExportStatusTextBlock.Text = result.Message;

                if (result.Success)
                {
                    MessageBox.Show(result.Message, "Export Recording", MessageBoxButton.OK, MessageBoxImage.Information);
                }
                else
                {
                    MessageBox.Show(result.Message + BuildLogSuffix(result.Log), "Export Recording", MessageBoxButton.OK, MessageBoxImage.Error);
                }
            }
            catch (Exception ex)
            {
                DebugLog.Write("[ExportScreen] export ERROR:\n" + ex);
                ExportStatusTextBlock.Text = "Export failed.";
                MessageBox.Show("Export failed.\n\n" + ex.Message, "Export Recording", MessageBoxButton.OK, MessageBoxImage.Error);
            }
            finally
            {
                _exportCancellation?.Dispose();
                _exportCancellation = null;
                SetExportingState(false);
            }
        }

        private void CancelButton_Click(object sender, RoutedEventArgs e)
        {
            if (_isExporting)
            {
                _exportCancellation?.Cancel();
                ExportStatusTextBlock.Text = "Cancelling export...";
                return;
            }

            Close();
        }

        private void HeaderDragArea_MouseLeftButtonDown(object sender, MouseButtonEventArgs e)
        {
            if (e.LeftButton != MouseButtonState.Pressed)
                return;

            try
            {
                DragMove();
            }
            catch
            {
            }
        }

        private void CloseButton_Click(object sender, RoutedEventArgs e)
        {
            if (_isExporting)
            {
                var result = MessageBox.Show(
                    "An export is still running. Do you want to cancel it?",
                    "Export Recording",
                    MessageBoxButton.YesNo,
                    MessageBoxImage.Warning);

                if (result != MessageBoxResult.Yes)
                    return;

                _exportCancellation?.Cancel();
                return;
            }

            Close();
        }

        private void UpdateOutputPathForMode()
        {
            if (_sessionInfo == null)
                return;

            if (_mode == RecordingExportMode.SingleFile)
                OutputPathTextBox.Text = ExportService.BuildDefaultSingleFileOutputPath(_sessionInfo.ProjectFolder, GetSelectedContainer());
            else
                OutputPathTextBox.Text = ExportService.BuildDefaultSeparateTracksOutputFolder(_sessionInfo.ProjectFolder);
        }

        private void UpdateSummary()
        {
            if (_sessionInfo == null)
                return;

            if (_mode == RecordingExportMode.SingleFile)
            {
                var layout = GetSelectedComboText(VideoLayoutComboBox);
                var audio = GetSelectedComboText(AudioMixComboBox);
                var container = GetSelectedComboText(ContainerComboBox);
                var quality = GetSelectedComboText(QualityComboBox);

                SummaryTextBlock.Text =
                    "Single file export\n" +
                    $"Video: {layout}\n" +
                    $"Audio: {audio}\n" +
                    $"Container: {container}\n" +
                    $"Quality: {quality}";
            }
            else
            {
                SummaryTextBlock.Text =
                    "Separate tracks export\n" +
                    $"Screen: {(ExportScreenCheckBox.IsChecked == true ? "enabled" : "disabled")}\n" +
                    $"Webcam: {(ExportWebcamCheckBox.IsChecked == true ? "enabled" : "disabled")}\n" +
                    $"Audio mix: {(ExportAudioMixCheckBox.IsChecked == true ? "enabled" : "disabled")}";
            }
        }

        private RecordingExportRequest BuildRequest()
        {
            if (_sessionInfo == null)
                throw new InvalidOperationException("Session information was not loaded.");

            return new RecordingExportRequest
            {
                ProjectFolder = _sessionInfo.ProjectFolder,
                OutputPath = OutputPathTextBox.Text,
                Mode = _mode,
                VideoLayout = GetSelectedVideoLayout(),
                AudioMode = GetSelectedAudioMode(),
                Container = GetSelectedContainer(),
                Quality = GetSelectedQuality(),
                ExportScreenTrack = ExportScreenCheckBox.IsChecked == true,
                ExportWebcamTrack = ExportWebcamCheckBox.IsChecked == true,
                ExportAudioMixTrack = ExportAudioMixCheckBox.IsChecked == true
            };
        }

        private string ValidateRequest(RecordingExportRequest request)
        {
            if (_sessionInfo == null)
                return "Session information was not loaded.";

            if (!File.Exists(ExportService.FindFFmpegPath()))
                return "FFmpeg was not found. Please place ffmpeg.exe inside Native\\FFmpeg\\ffmpeg.exe.";

            if (string.IsNullOrWhiteSpace(request.OutputPath))
                return "Please choose an output path.";

            if (request.Mode == RecordingExportMode.SingleFile)
            {
                if (request.VideoLayout == RecordingExportVideoLayout.NoVideo &&
                    request.AudioMode == RecordingExportAudioMode.NoAudio)
                {
                    return "Please choose at least one audio or video source to export.";
                }

                if ((request.Container == RecordingExportContainer.Mp3 ||
                     request.Container == RecordingExportContainer.Wav) &&
                    request.VideoLayout != RecordingExportVideoLayout.NoVideo)
                {
                    return "MP3 and WAV exports are audio-only. Please set Video layout to 'No video · Audio only'.";
                }

                if (request.VideoLayout == RecordingExportVideoLayout.NoVideo &&
                    request.AudioMode == RecordingExportAudioMode.NoAudio)
                {
                    return "Audio-only export requires at least one audio track. Please choose an audio mix.";
                }

                if (request.VideoLayout == RecordingExportVideoLayout.ScreenOnly && !_sessionInfo.HasScreen)
                    return "This recording does not contain a screen track.";

                if (request.VideoLayout == RecordingExportVideoLayout.WebcamOnly && !_sessionInfo.HasWebcam)
                    return "This recording does not contain a webcam track.";

                if ((request.VideoLayout == RecordingExportVideoLayout.ScreenWebcamPictureInPicture ||
                     request.VideoLayout == RecordingExportVideoLayout.ScreenWebcamSideBySide) &&
                    (!_sessionInfo.HasScreen || !_sessionInfo.HasWebcam))
                    return "This layout requires both screen and webcam tracks.";

                if (request.AudioMode == RecordingExportAudioMode.MicrophoneOnly && !_sessionInfo.HasMic)
                    return "This recording does not contain a microphone track.";

                if (request.AudioMode == RecordingExportAudioMode.SystemAudioOnly && !_sessionInfo.HasSystemAudio)
                    return "This recording does not contain a system audio track.";

                if (request.AudioMode == RecordingExportAudioMode.MicrophoneAndSystemAudio && (!_sessionInfo.HasMic || !_sessionInfo.HasSystemAudio))
                    return "This audio mix requires both microphone and system audio tracks.";
            }
            else
            {
                bool anySelected = request.ExportScreenTrack || request.ExportWebcamTrack || request.ExportAudioMixTrack;

                if (!anySelected)
                    return "Please select at least one track to export.";
            }

            return string.Empty;
        }

        private void SetExportingState(bool isExporting)
        {
            _isExporting = isExporting;

            ExportProgressBar.Visibility = isExporting ? Visibility.Visible : Visibility.Collapsed;
            StartExportButton.IsEnabled = !isExporting;
            StartExportButton.Content = isExporting ? "Exporting..." : "Start Export";
            CancelButton.Content = isExporting ? "Cancel Export" : "Cancel";

            SingleModeCard.IsEnabled = !isExporting;
            TracksModeCard.IsEnabled = !isExporting;
            VideoLayoutComboBox.IsEnabled = !isExporting;
            AudioMixComboBox.IsEnabled = !isExporting;
            ContainerComboBox.IsEnabled = !isExporting;
            QualityComboBox.IsEnabled = !isExporting;
            OutputPathTextBox.IsEnabled = !isExporting;
        }

        private void UpdateContainerOptionsForCurrentLayout(bool forceAudioDefault)
        {
            var layout = GetSelectedVideoLayout();
            bool isAudioOnly = layout == RecordingExportVideoLayout.NoVideo;

            SetComboBoxItemEnabled(ContainerComboBox, "Mp4", true);
            SetComboBoxItemEnabled(ContainerComboBox, "Mkv", true);
            SetComboBoxItemEnabled(ContainerComboBox, "Mp3", isAudioOnly);
            SetComboBoxItemEnabled(ContainerComboBox, "Wav", isAudioOnly);

            var current = GetSelectedContainer();

            if (!isAudioOnly && (current == RecordingExportContainer.Mp3 || current == RecordingExportContainer.Wav))
            {
                SelectComboBoxTag(ContainerComboBox, "Mp4");
                return;
            }

            if (isAudioOnly && forceAudioDefault)
                SelectComboBoxTag(ContainerComboBox, "Mp3");
        }

        private void UpdateQualityOptionsForCurrentContainer(bool forceDefault)
        {
            var selectedContainer = GetSelectedContainer();
            var selectedLayout = GetSelectedVideoLayout();
            bool isAudioOnly = selectedLayout == RecordingExportVideoLayout.NoVideo;

            if (selectedContainer == RecordingExportContainer.Wav)
            {
                ReplaceQualityItems(
                    ("WAV · 44.1 kHz · 16-bit", "Wav44100Hz16Bit"),
                    ("WAV · 48 kHz · 16-bit", "Wav48000Hz16Bit"),
                    ("WAV · 48 kHz · 24-bit", "Wav48000Hz24Bit")
                );

                if (forceDefault)
                    SelectComboBoxTag(QualityComboBox, "Wav48000Hz16Bit");

                return;
            }

            if (isAudioOnly || selectedContainer == RecordingExportContainer.Mp3)
            {
                ReplaceQualityItems(
                    ("Audio · 128 kbps", "Audio128Kbps"),
                    ("Audio · 192 kbps · Recommended", "Audio192Kbps"),
                    ("Audio · 320 kbps · High quality", "Audio320Kbps")
                );

                if (forceDefault)
                    SelectComboBoxTag(QualityComboBox, "Audio192Kbps");

                return;
            }

            ReplaceQualityItems(
                ("Fast · Smaller file", "Fast"),
                ("Balanced · Recommended", "Balanced"),
                ("High quality", "HighQuality")
            );

            if (forceDefault)
                SelectComboBoxTag(QualityComboBox, "Balanced");
        }

        private void ReplaceQualityItems(params (string Content, string Tag)[] items)
        {
            var previousTag = (QualityComboBox.SelectedItem as ComboBoxItem)?.Tag?.ToString();

            QualityComboBox.Items.Clear();

            foreach (var item in items)
            {
                QualityComboBox.Items.Add(new ComboBoxItem
                {
                    Content = item.Content,
                    Tag = item.Tag
                });
            }

            if (!string.IsNullOrWhiteSpace(previousTag))
                SelectComboBoxTag(QualityComboBox, previousTag);

            if (QualityComboBox.SelectedIndex < 0 && QualityComboBox.Items.Count > 0)
                QualityComboBox.SelectedIndex = 0;
        }

        private static string GetContainerExtension(RecordingExportContainer container)
        {
            return container switch
            {
                RecordingExportContainer.Mkv => ".mkv",
                RecordingExportContainer.Mp3 => ".mp3",
                RecordingExportContainer.Wav => ".wav",
                _ => ".mp4"
            };
        }

        private static string BuildSingleFileDialogFilter(RecordingExportContainer container)
        {
            return container switch
            {
                RecordingExportContainer.Mkv => "Matroska Video (*.mkv)|*.mkv|All files (*.*)|*.*",
                RecordingExportContainer.Mp3 => "MP3 Audio (*.mp3)|*.mp3|All files (*.*)|*.*",
                RecordingExportContainer.Wav => "WAV Audio (*.wav)|*.wav|All files (*.*)|*.*",
                _ => "MP4 Video (*.mp4)|*.mp4|All files (*.*)|*.*"
            };
        }

        private RecordingExportVideoLayout GetSelectedVideoLayout()
        {
            return ParseEnumTag(VideoLayoutComboBox, RecordingExportVideoLayout.NoVideo);
        }

        private RecordingExportAudioMode GetSelectedAudioMode()
        {
            return ParseEnumTag(AudioMixComboBox, RecordingExportAudioMode.NoAudio);
        }

        private RecordingExportContainer GetSelectedContainer()
        {
            return ParseEnumTag(ContainerComboBox, RecordingExportContainer.Mp4);
        }

        private RecordingExportQuality GetSelectedQuality()
        {
            return ParseEnumTag(QualityComboBox, RecordingExportQuality.Balanced);
        }

        private static TEnum ParseEnumTag<TEnum>(System.Windows.Controls.ComboBox comboBox, TEnum fallback) where TEnum : struct
        {
            if (comboBox.SelectedItem is ComboBoxItem item && item.Tag is string tag && Enum.TryParse<TEnum>(tag, out var parsed))
                return parsed;

            return fallback;
        }

        private static string GetSelectedComboText(System.Windows.Controls.ComboBox comboBox)
        {
            if (comboBox.SelectedItem is ComboBoxItem item)
                return item.Content?.ToString() ?? "—";

            return "—";
        }

        private static void SelectComboBoxTag(System.Windows.Controls.ComboBox comboBox, string tag)
        {
            foreach (ComboBoxItem item in comboBox.Items)
            {
                if (string.Equals(item.Tag?.ToString(), tag, StringComparison.OrdinalIgnoreCase))
                {
                    comboBox.SelectedItem = item;
                    return;
                }
            }
        }

        private static void SetComboBoxItemEnabled(System.Windows.Controls.ComboBox comboBox, string tag, bool isEnabled)
        {
            foreach (ComboBoxItem item in comboBox.Items)
            {
                if (string.Equals(item.Tag?.ToString(), tag, StringComparison.OrdinalIgnoreCase))
                {
                    item.IsEnabled = isEnabled;
                    return;
                }
            }
        }

        private static string ResolveInitialDirectory(string path)
        {
            try
            {
                var dir = Path.GetDirectoryName(path);
                if (!string.IsNullOrWhiteSpace(dir) && Directory.Exists(dir))
                    return dir;
            }
            catch { }

            return Environment.GetFolderPath(Environment.SpecialFolder.MyVideos);
        }

        private static string BuildLogSuffix(string log)
        {
            if (string.IsNullOrWhiteSpace(log))
                return string.Empty;

            var trimmed = log.Length > 2000 ? log.Substring(log.Length - 2000) : log;
            return "\n\nFFmpeg log:\n" + trimmed;
        }

        private static ImageSource? LoadAssetImage(string relativePath)
        {
            try
            {
                var image = new BitmapImage();
                image.BeginInit();
                image.UriSource = new Uri($"pack://application:,,,/{relativePath}", UriKind.Absolute);
                image.CacheOption = BitmapCacheOption.OnLoad;
                image.CreateOptions = BitmapCreateOptions.PreservePixelFormat;
                image.EndInit();
                image.Freeze();
                return image;
            }
            catch (Exception ex)
            {
                DebugLog.Write($"[ExportScreen] unable to load track icon {relativePath}: {ex.Message}");
                return null;
            }
        }

        private static System.Windows.Media.Brush BrushFrom(string hex)
        {
            return (System.Windows.Media.Brush)new BrushConverter().ConvertFromString(hex)!;
        }
    }
}

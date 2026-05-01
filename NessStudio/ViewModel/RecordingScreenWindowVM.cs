using DirectShowLib;
using NAudio.CoreAudioApi;
using NessStudio.Components.Header;
using NessStudio.Components.Menu;
using NessStudio.Components.RecentProjects;
using NessStudio.Models;
using NessStudio.Recording.Engines;
using NessStudio.Recording.Windows;
using NessStudio.View.DrawAreaScreen;
using NessStudio.View.HomeScreen;
using NessStudio.View.RecordingScreen;
using NessStudio.View.SavingRecordingScreen;
using NessStudio.ViewModel.Commands;
using NessStudio.ViewModel.Helpers;
using System;
using System.Collections.Generic;
using System.Collections.ObjectModel;
using System.ComponentModel;
using System.Diagnostics;
using System.Drawing;
using System.Drawing.Imaging;
using System.IO;
using System.Linq;
using System.Runtime.InteropServices;
using System.Text;
using System.Threading.Tasks;
using System.Windows;
using System.Windows.Forms;
using System.Windows.Input;
using System.Windows.Interop;
using System.Windows.Media;
using System.Windows.Media.Imaging;
using System.Windows.Threading;

namespace NessStudio.ViewModel

{
    public class RecordingScreenWindowVM : INotifyPropertyChanged
    {
        public event PropertyChangedEventHandler PropertyChanged;
        protected virtual void OnPropertyChanged(string propertyName)
        {
            PropertyChanged?.Invoke(this, new PropertyChangedEventArgs(propertyName));
        }
        public RecordingScreenWindow RecordingScreenWindow { get; set; }
        private CancellationTokenSource _screenCts;
        private volatile int _screenPending = 0;
        private int _screenRev = 0;
        private DateTime? _screenPreviewStartedAtUtc;
        private int _screenPreviewFirstFrameLogged = 0;
        private bool _uiPreviewSuppressedDuringRecording = false;
        private bool _pauseBusy;
        private bool _stopStarted = false;
        private bool _isSavingRecording = false;
        private bool _suppressSelectionHandler = false;
        private CaptureOverlayWindow? _captureOverlayWindow;
        private System.Windows.Rect? _activeDrawAreaOverlayCrop;
        public bool IsScreenEnabled { get => _isScreenEnabled; set { _isScreenEnabled = value; OnPropertyChanged(nameof(IsScreenEnabled)); if (_isLoaded) UpdateScreenPreviewRunning(); DrawAreaInfoText = string.Empty;IsDrawAreaInfoVisible = false; SelectPrimaryScreen(); } }
        public bool IsWebcamEnabled
        {
            get => _isWebcamEnabled;
            set { _isWebcamEnabled = value; OnPropertyChanged(nameof(IsWebcamEnabled)); if (_isLoaded) UpdateWebcamPreviewRunning(); }
        }
        private bool _isScreenEnabled = false;
        private bool _isWebcamEnabled = false;
        private bool _isMicrophoneEnabled = false;
        public bool IsMicrophoneEnabled
        {
            get => _isMicrophoneEnabled;
            set
            {
                if (_isMicrophoneEnabled != value)
                {
                    _isMicrophoneEnabled = value;
                    OnPropertyChanged(nameof(IsMicrophoneEnabled));
                    MicrophoneIcon = _isMicrophoneEnabled
                        ? "/Assets/Images/microfone-icon.png"
                        : "/Assets/Images/microfone-closed-icon.png";
                }
            }
        }
        private bool _isSystemAudioEnabled = false;
        public bool IsSystemAudioEnabled
        {
            get => _isSystemAudioEnabled;
            set
            {
                if (_isSystemAudioEnabled != value)
                {
                    _isSystemAudioEnabled = value;
                    OnPropertyChanged(nameof(IsSystemAudioEnabled));
                    SystemAudioIcon = _isSystemAudioEnabled
                        ? "/Assets/Images/system-audio-icon.png"
                        : "/Assets/Images/system-audio-closed-icon.png";
                }
            }
        }
        private string _drawAreaInfoText = string.Empty;
        public string DrawAreaInfoText
        {
            get => _drawAreaInfoText;
            set { _drawAreaInfoText = value; OnPropertyChanged(nameof(DrawAreaInfoText)); }
        }
        private bool _isDrawAreaInfoVisible = false;
        public bool IsDrawAreaInfoVisible
        {
            get => _isDrawAreaInfoVisible;
            set { _isDrawAreaInfoVisible = value; OnPropertyChanged(nameof(IsDrawAreaInfoVisible)); }
        }
        private bool _isRecButtonVisible = true;
        public bool IsRecButtonVisible
        {
            get => _isRecButtonVisible;
            set { _isRecButtonVisible = value; OnPropertyChanged(nameof(IsRecButtonVisible)); }
        }
        private bool _isEditEnabled = true;
        public bool IsEditEnabled
        {
            get => _isEditEnabled;
            set
            {
                if (_isEditEnabled != value)
                {
                    _isEditEnabled = value;
                    OnPropertyChanged(nameof(IsEditEnabled));
                }
            }
        }
        public string OutDirFolder = null;
        private string textRecTimer = "00:00:00";
        public string TextRecTimer
        {
            get => textRecTimer;
            set
            {
                textRecTimer = value;
                OnPropertyChanged("TextRecTimer");
            }
        }
        private string countdownText;
        public string CountdownText
        {
            get => countdownText;
            set
            {
                countdownText = value;
                OnPropertyChanged("CountdownText");
            }
        }
        private bool isCountdownVisible;
        public bool IsCountdownVisible
        {
            get => isCountdownVisible;
            set
            {
                isCountdownVisible = value;
                OnPropertyChanged("IsCountdownVisible");
            }
        }
        private bool isRecordingPanelVisible = false;
        public bool IsRecordingPanelVisible
        {
            get => isRecordingPanelVisible;
            set
            {
                isRecordingPanelVisible = value;
                OnPropertyChanged("IsRecordingPanelVisible");
            }
        }
        private bool isSettingsPanelVisible = true;
        public bool IsSettingsPanelVisible
        {
            get => isSettingsPanelVisible;
            set
            {
                isSettingsPanelVisible = value;
                OnPropertyChanged("IsSettingsPanelVisible");
            }
        }
        private bool _isSettingsMenuOpen;
        public bool IsSettingsMenuOpen
        {
            get => _isSettingsMenuOpen;
            set
            {
                if (_isSettingsMenuOpen == value)
                    return;
                _isSettingsMenuOpen = value;
                OnPropertyChanged(nameof(IsSettingsMenuOpen));
            }
        }
        private bool _isFpsSubmenuOpen;
        public bool IsFpsSubmenuOpen
        {
            get => _isFpsSubmenuOpen;
            set
            {
                if (_isFpsSubmenuOpen == value)
                    return;
                _isFpsSubmenuOpen = value;
                OnPropertyChanged(nameof(IsFpsSubmenuOpen));
            }
        }
        private bool _isTimerSubmenuOpen;
        public bool IsTimerSubmenuOpen
        {
            get => _isTimerSubmenuOpen;
            set
            {
                if (_isTimerSubmenuOpen == value)
                    return;
                _isTimerSubmenuOpen = value;
                OnPropertyChanged(nameof(IsTimerSubmenuOpen));
            }
        }
        private int _selectedRecordingFps = 30;
        public int SelectedRecordingFps
        {
            get => _selectedRecordingFps;
            set
            {
                int normalized = RecordingPreferencesService.NormalizeFps(value);
                if (_selectedRecordingFps == normalized)
                    return;
                _selectedRecordingFps = normalized;
                OnPropertyChanged(nameof(SelectedRecordingFps));
                OnPropertyChanged(nameof(CurrentSettingsSummary));
            }
        }
        private int _selectedRecordingTimerSeconds = 3;
        public int SelectedRecordingTimerSeconds
        {
            get => _selectedRecordingTimerSeconds;
            set
            {
                int normalized = RecordingPreferencesService.NormalizeTimer(value);
                if (_selectedRecordingTimerSeconds == normalized)
                    return;
                _selectedRecordingTimerSeconds = normalized;
                OnPropertyChanged(nameof(SelectedRecordingTimerSeconds));
                OnPropertyChanged(nameof(CurrentSettingsSummary));
            }
        }
        public string CurrentSettingsSummary => $"{SelectedRecordingFps} fps • {SelectedRecordingTimerSeconds}s";
        public ObservableCollection<RecordingPresetOption> FpsOptions { get; } = new();
        public ObservableCollection<RecordingPresetOption> TimerOptions { get; } = new();
        private string pauseResumeText = "Pause";
        public string PauseResumeText
        {
            get => pauseResumeText;
            set
            {
                pauseResumeText = value;
                OnPropertyChanged("PauseResumeText");
            }
        }
        public sealed class ScreenOption
        {
            public string Display { get; set; }
            public Screen Value { get; set; } 
            public bool IsEnabled { get; set; } = true;
        }
        public ObservableCollection<ScreenOption> ScreenOptions { get; } = new();
        private ScreenOption _selectedScreenOption;
        public ScreenOption SelectedScreenOption
        {
            get => _selectedScreenOption;
            set
            {
                if (_suppressSelectionHandler)
                {
                    _selectedScreenOption = value;
                    OnPropertyChanged(nameof(SelectedScreenOption));
                    SelectedScreen = value?.Value;
                    return;
                }
                if (value != null && value.Value == null &&
                    string.Equals(value.Display, DrawAreaLabel, StringComparison.OrdinalIgnoreCase))
                {
                    StartDrawAreaSelection();
                    return;
                }
                _selectedScreenOption = value;
                OnPropertyChanged(nameof(SelectedScreenOption));
                IsDrawAreaInfoVisible = false;
                DrawAreaInfoText = string.Empty;
                SelectedScreen = value?.Value;
            }
        }
        private System.Windows.Rect? _lastDrawArea;
        public Screen SelectedScreen { get => _selectedScreen; set { _selectedScreen = value; OnPropertyChanged(nameof(SelectedScreen)); if (_isLoaded) RestartScreenPreview(); } }
        public DsDevice SelectedWebcam { get => _selectedWebcam; set { _selectedWebcam = value; OnPropertyChanged(nameof(SelectedWebcam)); if (_isLoaded) RestartWebcamPreview(); } }
        public MMDevice SelectedMicrophone { get => _selectedMicrophone; set { _selectedMicrophone = value; OnPropertyChanged(nameof(SelectedMicrophone)); } }
        public MMDevice SelectedRenderLoopback { get => _selectedRender; set { _selectedRender = value; OnPropertyChanged(nameof(SelectedRenderLoopback)); } }
        private Screen _selectedScreen;
        private DsDevice _selectedWebcam;
        private MMDevice _selectedMicrophone;
        private MMDevice _selectedRender;
        private DispatcherTimer _screenTimer;
        private readonly object _screenBitmapLock = new();
        private ImageSource _screenPreviewImage;
        public ImageSource ScreenPreviewImage
        {
            get => _screenPreviewImage;
            set
            {
                if (_screenPreviewImage != value)
                {
                    _screenPreviewImage = value;
                    OnPropertyChanged(nameof(ScreenPreviewImage));
                }
            }
        }
        private ImageSource _webcamPreviewImage;
        public ImageSource WebcamPreviewImage
        {
            get => _webcamPreviewImage;
            set { _webcamPreviewImage = value; OnPropertyChanged(nameof(WebcamPreviewImage)); }
        }
        private string _systemAudioIcon = "/Assets/Images/system-audio-closed-icon.png";
        public string SystemAudioIcon
        {
            get => _systemAudioIcon;
            set
            {
                _systemAudioIcon = value;
                OnPropertyChanged(nameof(SystemAudioIcon));
            }
        }
        private string _microphoneIcon = "/Assets/Images/microfone-closed-icon.png";
        public string MicrophoneIcon
        {
            get => _microphoneIcon;
            set
            {
                _microphoneIcon = value;
                OnPropertyChanged(nameof(MicrophoneIcon));
            }
        }
        private string _buttonPlayPauseIcon = "/Assets/Images/pause-icon.png";
        public string ButtonPlayPauseIcon
        {
            get => _buttonPlayPauseIcon;
            set
            {
                _buttonPlayPauseIcon = value;
                OnPropertyChanged(nameof(ButtonPlayPauseIcon));
            }
        }
        private bool _isLoaded = false;
        private bool _isRecording = false;
        private bool _isPaused = false;
        private DispatcherTimer _recTimer;
        private readonly Stopwatch _recordingUiClock = new Stopwatch();
        private TimeSpan _elapsed;
        private bool _isClosing = false;
        private IRecorderEngine _rec;
        public ObservableCollection<System.Windows.Forms.Screen> Screens { get; } = new();
        public ObservableCollection<DirectShowLib.DsDevice> Webcams { get; } = new();
        public ObservableCollection<NAudio.CoreAudioApi.MMDevice> Microphones { get; } = new();
        public PlayRecodingCommand PlayRecodingCommand { get; set; }
        public PauseRecordingCommand PauseRecordingCommand { get; set; }
        public StopRecordingCommand StopRecordingCommand { get; set; }
        public DelegateCommand ToggleSettingsMenuCommand { get; set; }
        public DelegateCommand OpenFpsSubmenuCommand { get; set; }
        public DelegateCommand OpenTimerSubmenuCommand { get; set; }
        public DelegateCommand CloseSettingsMenusCommand { get; set; }
        public DelegateCommand SelectFpsOptionCommand { get; set; }
        public DelegateCommand SelectTimerOptionCommand { get; set; }
        private const string DrawAreaLabel = "Draw Area";
        public RecordingScreenWindowVM(RecordingScreenWindow recordingScreen)
        {
            RecordingScreenWindow = recordingScreen;
            PlayRecodingCommand = new PlayRecodingCommand(this);
            PauseRecordingCommand = new PauseRecordingCommand(this);
            StopRecordingCommand = new StopRecordingCommand(this);
            ToggleSettingsMenuCommand = new DelegateCommand(_ => ToggleSettingsMenu());
            OpenFpsSubmenuCommand = new DelegateCommand(_ => OpenFpsSubmenu());
            OpenTimerSubmenuCommand = new DelegateCommand(_ => OpenTimerSubmenu());
            CloseSettingsMenusCommand = new DelegateCommand(_ => CloseSettingsMenus());
            SelectFpsOptionCommand = new DelegateCommand(param => SelectFpsOption(param));
            SelectTimerOptionCommand = new DelegateCommand(param => SelectTimerOption(param));
            RecordingScreenWindow.Loaded += RecordingScreenWindow_Loaded;
            RecordingScreenWindow.Unloaded += RecordingScreenWindow_Unloaded;
            RecordingScreenWindow.Closing += RecordingScreenWindow_Closing;
            RecordingScreenWindow.Deactivated += (s, e) => CloseSettingsMenus();
            IsScreenEnabled = false;
            IsRecordingPanelVisible = false;
            IsSettingsPanelVisible = true;
            InitializeRecordingPreferencesUI();
        }

        private async void RecordingScreenWindow_Loaded(object sender, RoutedEventArgs e)
        {
            using var perf = RecordingPerfProbe.Scope("recording-window-loaded");

            try
            {
                RecordingPerfProbe.Mark("recording-window-loaded-begin");

                if (_recTimer == null)
                {
                    _recTimer = new DispatcherTimer(DispatcherPriority.Background);
                    _recTimer.Interval = TimeSpan.FromMilliseconds(250);
                    _recTimer.Tick += (s2, e2) =>
                    {
                        _elapsed = _recordingUiClock.Elapsed;
                        TextRecTimer = _elapsed.ToString(@"hh\:mm\:ss");
                    };
                }

                DebugLog.Write("[VM] RecordingScreenWindow_Loaded begin");

                await RecordingScreenWindow.Dispatcher.InvokeAsync(
                    () => { },
                    DispatcherPriority.Render);

                await Task.Yield();

                DebugLog.Write("[VM] RecordingScreenWindow_Loaded -> first render completed");
                RecordingPerfProbe.Mark("recording-window-first-render");

                LoadRecordingPreferencesFromApp();
                RefreshRecordingPresetSelections();

                await RecordingScreenWindow.Dispatcher.InvokeAsync(
                    () => { },
                    DispatcherPriority.Background);

                PopulateScreens();
                DebugLog.Write("[VM] RecordingScreenWindow_Loaded -> PopulateScreens end");
                RecordingPerfProbe.Mark("recording-window-screens-populated", $"screens={ScreenOptions.Count}");

                await RecordingScreenWindow.Dispatcher.InvokeAsync(
                    () => { },
                    DispatcherPriority.Background);

                PopulateWebcams();
                DebugLog.Write("[VM] RecordingScreenWindow_Loaded -> PopulateWebcams end");
                RecordingPerfProbe.Mark("recording-window-webcams-populated", $"webcams={Webcams.Count}");

                await RecordingScreenWindow.Dispatcher.InvokeAsync(
                    () => { },
                    DispatcherPriority.Background);

                PopulateMicrophonesAndSystem();
                DebugLog.Write("[VM] RecordingScreenWindow_Loaded -> PopulateMicrophonesAndSystem end");
                RecordingPerfProbe.Mark("recording-window-audio-populated", $"mics={Microphones.Count}");

                _isLoaded = true;

                await RecordingScreenWindow.Dispatcher.InvokeAsync(
                    () => { },
                    DispatcherPriority.Background);

                UpdateScreenPreviewRunning();
                DebugLog.Write("[VM] RecordingScreenWindow_Loaded -> UpdateScreenPreviewRunning end");

                await RecordingScreenWindow.Dispatcher.InvokeAsync(
                    () => { },
                    DispatcherPriority.Background);

                UpdateWebcamPreviewRunning();
                DebugLog.Write("[VM] RecordingScreenWindow_Loaded -> UpdateWebcamPreviewRunning end");

                RecordingPerfProbe.Mark(
                    "recording-window-previews-evaluated",
                    $"screenEnabled={IsScreenEnabled} | webcamEnabled={IsWebcamEnabled}");

                DebugLog.Write("[VM] RecordingScreenWindow_Loaded end");
                RecordingPerfProbe.Mark("recording-window-loaded-end");
            }
            catch (Exception ex)
            {
                DebugLog.Write("[VM] RecordingScreenWindow_Loaded ERROR:\n" + ex);
                RecordingPerfProbe.Mark("recording-window-loaded-error", ex.Message);

                System.Windows.MessageBox.Show(
                    $"Initialization error: {ex.Message}",
                    "Error",
                    MessageBoxButton.OK,
                    MessageBoxImage.Error);
            }
        }

        private void InitializeRecordingPreferencesUI()
        {
            BuildRecordingPresetCollections();
            LoadRecordingPreferencesFromApp();
            RefreshRecordingPresetSelections();
        }
        private void BuildRecordingPresetCollections()
        {
            FpsOptions.Clear();
            TimerOptions.Clear();
            FpsOptions.Add(new RecordingPresetOption { Label = "24 fps", Value = 24 });
            FpsOptions.Add(new RecordingPresetOption { Label = "25 fps", Value = 25 });
            FpsOptions.Add(new RecordingPresetOption { Label = "30 fps", Value = 30 });
            FpsOptions.Add(new RecordingPresetOption { Label = "48 fps", Value = 48 });
            FpsOptions.Add(new RecordingPresetOption { Label = "50 fps", Value = 50 });
            FpsOptions.Add(new RecordingPresetOption { Label = "60 fps", Value = 60 });
            TimerOptions.Add(new RecordingPresetOption { Label = "0s", Value = 0 });
            TimerOptions.Add(new RecordingPresetOption { Label = "3s", Value = 3 });
            TimerOptions.Add(new RecordingPresetOption { Label = "5s", Value = 5 });
            TimerOptions.Add(new RecordingPresetOption { Label = "10s", Value = 10 });
            TimerOptions.Add(new RecordingPresetOption { Label = "60s", Value = 60 });
        }
        private void LoadRecordingPreferencesFromApp()
        {
            var app = (App)System.Windows.Application.Current;
            SelectedRecordingTimerSeconds = RecordingPreferencesService.NormalizeTimer(app.RecordingTimerSeconds);
            SelectedRecordingFps = RecordingPreferencesService.NormalizeFps(app.RecordingFps);
        }
        private void RefreshRecordingPresetSelections()
        {
            foreach (var option in FpsOptions)
                option.IsSelected = option.Value == SelectedRecordingFps;
            foreach (var option in TimerOptions)
                option.IsSelected = option.Value == SelectedRecordingTimerSeconds;
            OnPropertyChanged(nameof(CurrentSettingsSummary));
        }
        private bool CanChangeRecordingPreferences()
        {
            return IsEditEnabled && !_isRecording && !_isSavingRecording && !_stopStarted;
        }
        private void ToggleSettingsMenu()
        {
            if (!CanChangeRecordingPreferences())
                return;
            IsSettingsMenuOpen = !IsSettingsMenuOpen;
            if (!IsSettingsMenuOpen)
            {
                IsFpsSubmenuOpen = false;
                IsTimerSubmenuOpen = false;
            }
        }
        private void OpenFpsSubmenu()
        {
            if (!CanChangeRecordingPreferences())
                return;
            IsSettingsMenuOpen = true;
            IsFpsSubmenuOpen = true;
            IsTimerSubmenuOpen = false;
        }
        private void OpenTimerSubmenu()
        {
            if (!CanChangeRecordingPreferences())
                return;
            IsSettingsMenuOpen = true;
            IsFpsSubmenuOpen = false;
            IsTimerSubmenuOpen = true;
        }
        private void CloseSettingsMenus()
        {
            IsSettingsMenuOpen = false;
            IsFpsSubmenuOpen = false;
            IsTimerSubmenuOpen = false;
        }
        private void SelectFpsOption(object parameter)
        {
            if (!CanChangeRecordingPreferences())
                return;
            int fps;
            if (parameter is int intValue)
                fps = intValue;
            else if (parameter is string strValue && int.TryParse(strValue, out var parsed))
                fps = parsed;
            else
                return;
            ApplyRecordingPreferencesSelection(fps: fps, timerSeconds: null);
        }
        private void SelectTimerOption(object parameter)
        {
            if (!CanChangeRecordingPreferences())
                return;
            int timer;
            if (parameter is int intValue)
                timer = intValue;
            else if (parameter is string strValue && int.TryParse(strValue, out var parsed))
                timer = parsed;
            else
                return;
            ApplyRecordingPreferencesSelection(fps: null, timerSeconds: timer);
        }
        private void ApplyRecordingPreferencesSelection(int? fps, int? timerSeconds)
        {
            if (fps.HasValue)
                SelectedRecordingFps = RecordingPreferencesService.NormalizeFps(fps.Value);
            if (timerSeconds.HasValue)
                SelectedRecordingTimerSeconds = RecordingPreferencesService.NormalizeTimer(timerSeconds.Value);
            var app = (App)System.Windows.Application.Current;
            app.RecordingFps = SelectedRecordingFps;
            app.RecordingTimerSeconds = SelectedRecordingTimerSeconds;
            RefreshRecordingPresetSelections();
            SaveRecordingPreferencesToDisk();
            if (fps.HasValue)
                DebugLog.Write($"[REC-PREFS] fps changed -> {SelectedRecordingFps}");
            if (timerSeconds.HasValue)
                DebugLog.Write($"[REC-PREFS] timer changed -> {SelectedRecordingTimerSeconds}");
            CloseSettingsMenus();
        }
        private void SaveRecordingPreferencesToDisk()
        {
            var service = new RecordingPreferencesService();
            var current = service.Load();
            current.RecordingFps = SelectedRecordingFps;
            current.TimerSeconds = SelectedRecordingTimerSeconds;
            service.Save(current);
        }
        private RecordingRuntimeOptions BuildRuntimeOptions()
        {
            return new RecordingRuntimeOptions
            {
                RecordingFps = SelectedRecordingFps,
                CountdownSeconds = SelectedRecordingTimerSeconds
            };
        }
        private void RecordingScreenWindow_Unloaded(object sender, RoutedEventArgs e)
        {
            StopScreenPreview();
            StopWebcamPreview();

            try
            {
                MediaCaptureWebcamSession.Shared.ReleaseAsync().GetAwaiter().GetResult();
            }
            catch
            {
            }
        }
        private void RecordingScreenWindow_Closing(object? sender, CancelEventArgs e)
        {
            if (_isClosing)
            {
                return;
            }
            if (_stopStarted || _isSavingRecording)
            {
                e.Cancel = true;
                DebugLog.Write("[SAVEUI] close request ignored while saving");
                return;
            }

            ClearDrawAreaOverlay();
            StopScreenPreview();
            StopWebcamPreview();

            try
            {
                MediaCaptureWebcamSession.Shared.ReleaseAsync().GetAwaiter().GetResult();
            }
            catch
            {
            }
            if (_isRecording)
            {
                var result = System.Windows.MessageBox.Show(
                    "A recording is in progress.\nDo you want to stop and exit?",
                    "Stop recording and exit?",
                    MessageBoxButton.YesNo,
                    MessageBoxImage.Question,
                    MessageBoxResult.No
                );
                if (result != MessageBoxResult.Yes)
                {
                    e.Cancel = true;
                    DebugLog.Write("[VM] close canceled by user while recording");
                    return;
                }
            }
            e.Cancel = true;
            _isClosing = true;
            try { RecordingScreenWindow.IsEnabled = false; } catch { }
            RecordingScreenWindow.Dispatcher.BeginInvoke(async () =>
            {
                try
                {
                    try { _recTimer?.Stop(); } catch { }
                    try { _recordingUiClock.Stop(); } catch { }
                    if (_isRecording && _rec != null)
                    {
                        DebugLog.Write("[VM] close flow -> StopAsync begin");
                        await _rec.StopAsync();
                        DebugLog.Write("[VM] close flow -> StopAsync end");
                    }
                }
                catch (Exception ex)
                {
                    DebugLog.Write("[VM] close flow ERROR:\n" + ex);
                }
                finally
                {
                    try { _rec?.Dispose(); } catch { }
                    _rec = null;
                    _isRecording = false;
                    _isPaused = false;
                }
                try
                {
                    DebugLog.Write("[VM] close flow -> window close");
                    RecordingScreenWindow.Close();
                }
                catch (Exception ex)
                {
                    DebugLog.Write("[VM] close flow close ERROR:\n" + ex);
                }
            }, DispatcherPriority.Normal);
        }
        private void PopulateScreens()
        {
            ScreenOptions.Clear();
            var all = Screen.AllScreens.ToList();
            ScreenOptions.Add(new ScreenOption
            {
                Display = DrawAreaLabel,
                Value = null,
                IsEnabled = true
            });
            foreach (var scr in all)
            {
                var b = scr.Bounds;
                string friendly = GetDisplayFriendlyName(scr.DeviceName);
                string label = $"{b.Width}x{b.Height} @ {b.X},{b.Y}" +
                               (scr.Primary ? " (Primary)" : "");
                ScreenOptions.Add(new ScreenOption
                {
                    Display = label,
                    Value = scr,
                    IsEnabled = true
                });
            }
            var primary = all.FirstOrDefault(s => s.Primary) ?? all.FirstOrDefault();
            SelectedScreenOption = ScreenOptions.FirstOrDefault(o => o.Value == primary)
                                   ?? ScreenOptions.FirstOrDefault();
        }
        private void PopulateWebcams()
        {
            Webcams.Clear();
            var cams = DirectShowLib.DsDevice.GetDevicesOfCat(DirectShowLib.FilterCategory.VideoInputDevice)
                       ?.ToList() ?? new List<DirectShowLib.DsDevice>();
            foreach (var cam in cams)
                Webcams.Add(cam);
            SelectedWebcam = cams.FirstOrDefault();
        }
        private void PopulateMicrophonesAndSystem()
        {
            Microphones.Clear();
            using var enumerator = new NAudio.CoreAudioApi.MMDeviceEnumerator();
            var mics = enumerator
                .EnumerateAudioEndPoints(NAudio.CoreAudioApi.DataFlow.Capture,
                                         NAudio.CoreAudioApi.DeviceState.Active)
                .ToList();
            foreach (var mic in mics)
                Microphones.Add(mic);
            NAudio.CoreAudioApi.MMDevice defaultMic = null;
            try
            {
                defaultMic = enumerator.GetDefaultAudioEndpoint(
                    NAudio.CoreAudioApi.DataFlow.Capture,
                    NAudio.CoreAudioApi.Role.Communications
                );
            }
            catch {  }
            if (defaultMic != null)
                SelectedMicrophone = mics.FirstOrDefault(m =>
                    string.Equals(m.ID, defaultMic.ID, StringComparison.OrdinalIgnoreCase))
                    ?? mics.FirstOrDefault();
            else
                SelectedMicrophone = mics.FirstOrDefault();
            NAudio.CoreAudioApi.MMDevice defRender = null;
            try
            {
                defRender = enumerator.GetDefaultAudioEndpoint(
                    NAudio.CoreAudioApi.DataFlow.Render,
                    NAudio.CoreAudioApi.Role.Multimedia
                );
            }
            catch { }
            SelectedRenderLoopback = defRender;
        }
        private void UpdateScreenPreviewRunning()
        {
            if (_uiPreviewSuppressedDuringRecording)
            {
                StopScreenPreview();
                return;
            }

            if (IsScreenEnabled && SelectedScreen != null)
                StartScreenPreview();
            else
                StopScreenPreview();
        }
        private void StartScreenPreview()
        {
            if (_screenCts != null) return;

            var scr = SelectedScreen ?? System.Windows.Forms.Screen.PrimaryScreen;
            var srcBounds = scr.Bounds;
            int W = srcBounds.Width;
            int H = srcBounds.Height;

            RecordingPerfProbe.Mark(
                "screen-preview-start-requested",
                $"device={(scr?.DeviceName ?? "unknown")} | size={W}x{H}");

            _screenPreviewStartedAtUtc = DateTime.UtcNow;
            System.Threading.Interlocked.Exchange(ref _screenPreviewFirstFrameLogged, 0);

            _screenCts = new CancellationTokenSource();
            var token = _screenCts.Token;
            int rev = System.Threading.Interlocked.Increment(ref _screenRev);

            Task.Run(() =>
            {
                using var bmpFull = new System.Drawing.Bitmap(W, H, System.Drawing.Imaging.PixelFormat.Format24bppRgb);
                using var gFull = System.Drawing.Graphics.FromImage(bmpFull);

                gFull.InterpolationMode = System.Drawing.Drawing2D.InterpolationMode.Low;
                gFull.CompositingMode = System.Drawing.Drawing2D.CompositingMode.SourceCopy;
                gFull.CompositingQuality = System.Drawing.Drawing2D.CompositingQuality.HighSpeed;
                gFull.PixelOffsetMode = System.Drawing.Drawing2D.PixelOffsetMode.HighSpeed;

                var sw = System.Diagnostics.Stopwatch.StartNew();
                long last = 0;

                while (!token.IsCancellationRequested)
                {
                    if (rev != System.Threading.Volatile.Read(ref _screenRev)) break;
                    if (sw.ElapsedMilliseconds - last < 100) { Thread.Sleep(1); continue; }

                    last = sw.ElapsedMilliseconds;

                    gFull.CopyFromScreen(
                        srcBounds.X, srcBounds.Y,
                        0, 0,
                        new System.Drawing.Size(W, H),
                        System.Drawing.CopyPixelOperation.SourceCopy
                    );

                    if (System.Threading.Interlocked.Exchange(ref _screenPending, 1) == 0)
                    {
                        IntPtr hBmp = bmpFull.GetHbitmap();

                        System.Windows.Application.Current?.Dispatcher?.BeginInvoke(new Action(() =>
                        {
                            try
                            {
                                if (rev != _screenRev) return;

                                var bs = System.Windows.Interop.Imaging.CreateBitmapSourceFromHBitmap(
                                    hBmp, IntPtr.Zero, Int32Rect.Empty, BitmapSizeOptions.FromEmptyOptions());

                                bs.Freeze();
                                ScreenPreviewImage = bs;

                                if (System.Threading.Interlocked.Exchange(ref _screenPreviewFirstFrameLogged, 1) == 0)
                                {
                                    double firstFrameAfterMs = _screenPreviewStartedAtUtc.HasValue
                                        ? (DateTime.UtcNow - _screenPreviewStartedAtUtc.Value).TotalMilliseconds
                                        : -1.0;

                                    RecordingPerfProbe.Mark(
                                        "screen-preview-first-frame",
                                        $"size={W}x{H} | after={firstFrameAfterMs:F0}ms");
                                }
                            }
                            finally
                            {
                                DeleteObject(hBmp);
                                System.Threading.Interlocked.Exchange(ref _screenPending, 0);
                            }
                        }), System.Windows.Threading.DispatcherPriority.Background);
                    }
                }
            }, token);
        }
        private void StopScreenPreview()
        {
            using var perf = RecordingPerfProbe.Scope(
                "screen-preview-stop",
                $"hadCts={_screenCts != null}");

            try
            {
                System.Threading.Interlocked.Increment(ref _screenRev);
                _screenCts?.Cancel();
                _screenCts?.Dispose();
                _screenCts = null;
                System.Threading.Interlocked.Exchange(ref _screenPending, 0);
            }
            catch
            {
            }

            _screenPreviewStartedAtUtc = null;
            System.Threading.Interlocked.Exchange(ref _screenPreviewFirstFrameLogged, 0);

            ScreenPreviewImage = null;

            RecordingPerfProbe.Mark("screen-preview-stopped");
        }
        private void RestartScreenPreview()
        {
            if (_uiPreviewSuppressedDuringRecording)
                return;

            StopScreenPreview();
            UpdateScreenPreviewRunning();
        }
        public void CaptureScreenFrame()
        {
            if (SelectedScreen == null) return;
            var bounds = SelectedScreen.Bounds;
            using var bmp = new Bitmap(bounds.Width, bounds.Height, System.Drawing.Imaging.PixelFormat.Format24bppRgb);
            using (var g = Graphics.FromImage(bmp))
            {
                g.CopyFromScreen(bounds.X, bounds.Y, 0, 0, bounds.Size, CopyPixelOperation.SourceCopy);
            }
            IntPtr hBmp = bmp.GetHbitmap();
            try
            {
                var src = Imaging.CreateBitmapSourceFromHBitmap(
                    hBmp,
                    IntPtr.Zero,
                    Int32Rect.Empty,
                    BitmapSizeOptions.FromEmptyOptions());
                src.Freeze();
                if (System.Windows.Application.Current?.Dispatcher?.CheckAccess() == true)
                {
                    ScreenPreviewImage = src;
                }
                else
                {
                    System.Windows.Application.Current?.Dispatcher?.BeginInvoke(new Action(() =>
                    {
                        ScreenPreviewImage = src;
                    }));
                }
            }
            finally
            {
                DeleteObject(hBmp);
            }
        }
        [DllImport("gdi32.dll")]
        static extern bool DeleteObject(IntPtr hObject);
        private void UpdateWebcamPreviewRunning()
        {
            if (_uiPreviewSuppressedDuringRecording)
            {
                StopWebcamPreview();
                return;
            }

            if (IsWebcamEnabled && SelectedWebcam != null)
                StartWebcamPreview();
            else
                StopWebcamPreview();
        }

        private async void StartWebcamPreview()
        {
            if (SelectedWebcam == null)
                return;

            using var perf = RecordingPerfProbe.Scope(
                "webcam-preview-start",
                $"webcam={SelectedWebcam.Name}");

            try
            {
                await StopWebcamPreviewAsync(clearImage: false);

                string camName = SelectedWebcam.Name;

                RecordingPerfProbe.Mark("webcam-preview-start-requested", $"webcam={camName}");
                DebugLog.Write($"[VM] StartWebcamPreview begin | webcam={camName}");

                await MediaCaptureWebcamSession.Shared.StartPreviewAsync(
                    camName,
                    frame =>
                    {
                        RecordingScreenWindow?.Dispatcher?.BeginInvoke(new Action(() =>
                        {
                            WebcamPreviewImage = frame;
                        }), DispatcherPriority.Render);
                    });

                DebugLog.Write("[VM] StartWebcamPreview end");
                RecordingPerfProbe.Mark(
                    "webcam-preview-running",
                    $"webcam={camName} | stable={MediaCaptureWebcamSession.Shared.IsPreviewStable}");
            }
            catch (Exception ex)
            {
                DebugLog.Write("[VM] StartWebcamPreview ERROR:\n" + ex);
                RecordingPerfProbe.Mark("webcam-preview-error", ex.Message);

                RecordingScreenWindow?.Dispatcher?.BeginInvoke(new Action(() =>
                {
                    WebcamPreviewImage = null;
                    System.Windows.MessageBox.Show(
                        $"Webcam error: {ex.Message}",
                        "Webcam",
                        MessageBoxButton.OK,
                        MessageBoxImage.Warning);
                }));
            }
        }
        private void StopWebcamPreview()
        {
            try
            {
                StopWebcamPreviewAsync(clearImage: true).GetAwaiter().GetResult();
            }
            catch
            {
            }
        }

        private async Task StopWebcamPreviewAsync(bool clearImage)
        {
            using var perf = RecordingPerfProbe.Scope(
                "webcam-preview-stop",
                $"clearImage={clearImage}");

            try
            {
                await MediaCaptureWebcamSession.Shared.StopPreviewAsync(clearCallback: true).ConfigureAwait(false);
            }
            catch (Exception ex)
            {
                DebugLog.Write("[VM] StopWebcamPreviewAsync warning:\n" + ex);
                RecordingPerfProbe.Mark("webcam-preview-stop-warning", ex.Message);
            }

            if (clearImage)
            {
                RecordingScreenWindow?.Dispatcher?.BeginInvoke(new Action(() =>
                {
                    WebcamPreviewImage = null;
                }));
            }

            RecordingPerfProbe.Mark("webcam-preview-stopped", $"clearImage={clearImage}");
        }

        private async Task DisableUiPreviewsForActiveRecordingAsync()
        {
            if (_uiPreviewSuppressedDuringRecording)
                return;

            _uiPreviewSuppressedDuringRecording = true;

            RecordingPerfProbe.Mark("recording-ui-preview-suppression-begin");

            StopScreenPreview();
            await StopWebcamPreviewAsync(clearImage: true);

            RecordingPerfProbe.Mark("recording-ui-preview-suppression-end");
        }

        private void ResetUiPreviewSuppression()
        {
            _uiPreviewSuppressedDuringRecording = false;
        }

        private async void RestartWebcamPreview()
        {
            if (_uiPreviewSuppressedDuringRecording)
                return;

            await StopWebcamPreviewAsync(clearImage: true);
            UpdateWebcamPreviewRunning();
        }

        public void HandleButtonAction(MenuAction action)
        {
            switch (action)
            {
                case MenuAction.StartRecording:
                    BtnRec_Click();
                    break;
                case MenuAction.PauseRecording:
                    BtnPauseResume_Click();
                    break;
                case MenuAction.StopRecording:
                    BtnStop_Click();
                    break;
            }
        }

        private void ShowDrawAreaOverlay(System.Windows.Rect crop)
        {
            try
            {
                _activeDrawAreaOverlayCrop = crop;

                CloseDrawAreaOverlayWindow();

                _captureOverlayWindow = new CaptureOverlayWindow(crop)
                {
                    Owner = RecordingScreenWindow,
                    ShowInTaskbar = false,
                    WindowStartupLocation = WindowStartupLocation.Manual
                };

                _captureOverlayWindow.Show();

                DebugLog.Write($"[VM] draw area overlay shown | crop={crop}");
            }
            catch (Exception ex)
            {
                DebugLog.Write("[VM] draw area overlay show ERROR:\n" + ex);
                _captureOverlayWindow = null;
            }
        }

        private void RestoreDrawAreaOverlayIfNeeded()
        {
            if (_activeDrawAreaOverlayCrop.HasValue)
                ShowDrawAreaOverlay(_activeDrawAreaOverlayCrop.Value);
        }

        private void CloseDrawAreaOverlayWindow()
        {
            try
            {
                if (_captureOverlayWindow != null)
                {
                    _captureOverlayWindow.Close();
                    DebugLog.Write("[VM] draw area overlay closed");
                }
            }
            catch (Exception ex)
            {
                DebugLog.Write("[VM] draw area overlay close ERROR:\n" + ex);
            }
            finally
            {
                _captureOverlayWindow = null;
            }
        }

        private void ClearDrawAreaOverlay()
        {
            CloseDrawAreaOverlayWindow();
            _activeDrawAreaOverlayCrop = null;
        }

        private Screen? ResolveScreenTargetForRecording(bool isDrawArea, System.Windows.Rect? crop)
        {
            if (!IsScreenEnabled)
                return null;

            if (isDrawArea)
            {
                if (!crop.HasValue)
                    return null;

                var screenFromCrop = FindSingleScreenContainingCrop(crop.Value);

                if (screenFromCrop != null)
                {
                    DebugLog.Write(
                        $"[VM] draw area screen target resolved | " +
                        $"device={screenFromCrop.DeviceName} | " +
                        $"bounds={screenFromCrop.Bounds} | " +
                        $"crop={crop.Value}");

                    return screenFromCrop;
                }

                DebugLog.Write(
                    $"[VM] draw area screen target invalid | " +
                    $"crop spans multiple monitors or is outside available screens | crop={crop.Value}");

                return null;
            }

            return SelectedScreen;
        }

        private Screen? FindSingleScreenContainingCrop(System.Windows.Rect crop)
        {
            var cropRect = ToGdiRectangle(crop);

            foreach (var screen in Screen.AllScreens)
            {
                if (screen.Bounds.Contains(cropRect))
                    return screen;
            }

            return null;
        }

        private static System.Drawing.Rectangle ToGdiRectangle(System.Windows.Rect crop)
        {
            int left = (int)Math.Floor(Math.Min(crop.Left, crop.Right));
            int top = (int)Math.Floor(Math.Min(crop.Top, crop.Bottom));
            int right = (int)Math.Ceiling(Math.Max(crop.Left, crop.Right));
            int bottom = (int)Math.Ceiling(Math.Max(crop.Top, crop.Bottom));

            int width = Math.Max(1, right - left);
            int height = Math.Max(1, bottom - top);

            return new System.Drawing.Rectangle(left, top, width, height);
        }

        private async void BtnRec_Click()
        {
            if (_isRecording) return;

            bool isDrawArea =
                SelectedScreenOption != null &&
                SelectedScreenOption.Value == null &&
                string.Equals(SelectedScreenOption.Display, DrawAreaLabel, StringComparison.OrdinalIgnoreCase);

            if (isDrawArea && !_lastDrawArea.HasValue)
            {
                System.Windows.MessageBox.Show(
                    "Select a draw area before starting the recording.",
                    "Recording",
                    MessageBoxButton.OK,
                    MessageBoxImage.Warning
                );
                return;
            }

            System.Windows.Rect? crop = isDrawArea ? _lastDrawArea.Value : (System.Windows.Rect?)null;
            Screen? screen = ResolveScreenTargetForRecording(isDrawArea, crop);

            if (isDrawArea && screen == null)
            {
                IsCountdownVisible = false;
                IsRecButtonVisible = true;
                IsEditEnabled = true;

                System.Windows.MessageBox.Show(
                    "The selected draw area spans multiple monitors or is outside the selected screen.\nPlease select an area inside a single monitor.",
                    "Recording",
                    MessageBoxButton.OK,
                    MessageBoxImage.Warning
                );

                return;
            }

            if (IsScreenEnabled && screen == null)
            {
                IsCountdownVisible = false;
                IsRecButtonVisible = true;
                IsEditEnabled = true;

                System.Windows.MessageBox.Show(
                    "Could not resolve a valid screen target. Please select a screen or a draw area again.",
                    "Recording",
                    MessageBoxButton.OK,
                    MessageBoxImage.Warning
                );

                return;
            }

            string outDir = Path.Combine(
                Environment.GetFolderPath(Environment.SpecialFolder.MyDocuments),
                "NessStudio",
                "Recordings",
                DateTime.Now.ToString("yyyyMMdd_HHmmss"));

            Directory.CreateDirectory(outDir);
            OutDirFolder = outDir;

            var paths = new NessStudio.Models.RecordingOutputPaths(outDir);

            try
            {
                var runtimeOptions = BuildRuntimeOptions();

                RecordingPerfProbe.Mark(
                "recording-start-requested",
                $"drawArea={isDrawArea} | screen={IsScreenEnabled} | webcam={IsWebcamEnabled} | mic={IsMicrophoneEnabled} | system={IsSystemAudioEnabled}");

                DebugLog.Write($"[VM] BtnRec_Click begin | isDrawArea={isDrawArea} | screenEnabled={IsScreenEnabled} | webcamEnabled={IsWebcamEnabled} | micEnabled={IsMicrophoneEnabled} | systemEnabled={IsSystemAudioEnabled}");
                DebugLog.Write($"[REC] runtime options | timer={runtimeOptions.CountdownSeconds} | fps={runtimeOptions.RecordingFps}");

                var selection = new NessStudio.Models.RecordingDeviceSelection(
                    webcamName: (IsWebcamEnabled && SelectedWebcam != null) ? SelectedWebcam.Name : null,
                    micDeviceId: (IsMicrophoneEnabled && SelectedMicrophone != null) ? SelectedMicrophone.ID : null,
                    loopbackDeviceId: (IsSystemAudioEnabled && SelectedRenderLoopback != null) ? SelectedRenderLoopback.ID : null,
                    displayFriendlyName: (screen != null) ? GetDisplayFriendlyName(screen.DeviceName) : null
                );

                if (!selection.AnyAudio && !selection.AnyVideo && !IsScreenEnabled && !isDrawArea)
                {
                    IsCountdownVisible = false;
                    IsRecButtonVisible = true;
                    IsEditEnabled = true;

                    System.Windows.MessageBox.Show(
                        "Select at least one video or audio source.",
                        "Recording",
                        MessageBoxButton.OK,
                        MessageBoxImage.Warning
                    );
                    return;
                }

                var targets = selection.BuildTargets(screen);

                DebugLog.Write($"[VM] Creating recorder | outDir={outDir} | crop={(crop.HasValue ? crop.Value.ToString() : "null")}");
                _rec = RecorderEngineFactory.Create(paths, targets, crop, runtimeOptions);

                CloseSettingsMenus();
                IsRecButtonVisible = false;
                IsEditEnabled = false;

                DebugLog.Write("[VM] Keeping screen preview alive only until real recording starts");
                DebugLog.Write("[VM] Keeping webcam UI preview alive only until real recording starts");

                DebugLog.Write("[VM] Calling _rec.PrepareAsync() during countdown");
                Task prepareTask = _rec.PrepareAsync();

                if (runtimeOptions.CountdownSeconds > 0)
                {
                    IsCountdownVisible = true;

                    for (int i = runtimeOptions.CountdownSeconds; i >= 1; i--)
                    {
                        CountdownText = i.ToString();
                        await Task.Delay(1000);
                    }

                    IsCountdownVisible = false;
                    CountdownText = string.Empty;
                }
                else
                {
                    IsCountdownVisible = false;
                    CountdownText = string.Empty;
                }

                DebugLog.Write("[VM] Awaiting _rec.PrepareAsync() completion");
                await prepareTask;
                RecordingPerfProbe.Mark("recording-prepare-finished");
                DebugLog.Write("[VM] _rec.PrepareAsync() succeeded");

                _isRecording = true;
                _isPaused = false;
                _elapsed = TimeSpan.Zero;
                _recordingUiClock.Reset();
                TextRecTimer = "00:00:00";
                PauseResumeText = "Pause";
                IsRecordingPanelVisible = true;
                IsSettingsPanelVisible = false;

                await System.Windows.Threading.Dispatcher.Yield(System.Windows.Threading.DispatcherPriority.Render);

                DebugLog.Write("[VM] Calling _rec.StartAsync()");
                RecordingPerfProbe.Mark("recording-start-dispatch");
                _recordingUiClock.Restart();
                _recTimer.Start();
                await _rec.StartAsync();

                DebugLog.Write("[VM] _rec.StartAsync() succeeded");
                RecordingPerfProbe.Mark("recording-start-succeeded");

                if (isDrawArea && crop.HasValue)
                    ShowDrawAreaOverlay(crop.Value);

                await DisableUiPreviewsForActiveRecordingAsync();

                IsEditEnabled = true;
                _elapsed = _recordingUiClock.Elapsed;
                TextRecTimer = _elapsed.ToString(@"hh\:mm\:ss");
                RecordingPerfProbe.Mark("recording-running");
            }
            catch (Exception ex)
            {
                DebugLog.Write("[VM] BtnRec_Click ERROR:\n" + ex);

                try { _recTimer.Stop(); } catch { }
                try { _recordingUiClock.Stop(); } catch { }
                try { _rec?.Dispose(); } catch { }

                _rec = null;
                _isRecording = false;
                _isPaused = false;
                _elapsed = TimeSpan.Zero;
                _recordingUiClock.Reset();

                ResetUiPreviewSuppression();
                ClearDrawAreaOverlay();

                IsCountdownVisible = false;
                CountdownText = string.Empty;
                IsRecordingPanelVisible = false;
                IsSettingsPanelVisible = true;
                IsRecButtonVisible = true;
                IsEditEnabled = true;
                TextRecTimer = "00:00:00";
                PauseResumeText = "Pause";

                try
                {
                    if (IsWebcamEnabled && SelectedWebcam != null)
                        StartWebcamPreview();
                }
                catch
                {
                }

                System.Windows.MessageBox.Show(
                    $"Failed to start recording:\n{ex.Message}\n\n{ex.InnerException?.Message}\n\nLog file:\n{DebugLog.GetPath()}",
                    "Recording",
                    MessageBoxButton.OK,
                    MessageBoxImage.Error
                );
            }
        }

        private async void BtnPauseResume_Click()
        {
            if (_pauseBusy || _stopStarted || _isSavingRecording || !_isRecording || _rec == null)
                return;

            _pauseBusy = true;

            try
            {
                if (!_isPaused)
                {
                    using var perf = RecordingPerfProbe.Scope("recording-pause");

                    _recTimer.Stop();
                    _recordingUiClock.Stop();
                    _elapsed = _recordingUiClock.Elapsed;
                    TextRecTimer = _elapsed.ToString(@"hh\:mm\:ss");
                    PauseResumeText = "Resume";
                    ButtonPlayPauseIcon = "/Assets/Images/play-icon.png";

                    await System.Windows.Threading.Dispatcher.Yield(System.Windows.Threading.DispatcherPriority.Render);

                    RecordingPerfProbe.Mark("recording-pause-dispatch");
                    await _rec.PauseAsync();
                    _isPaused = true;
                    CloseDrawAreaOverlayWindow();

                    RecordingPerfProbe.Mark("recording-paused");
                }
                else
                {
                    using var perf = RecordingPerfProbe.Scope("recording-resume");

                    PauseResumeText = "Pause";
                    ButtonPlayPauseIcon = "/Assets/Images/pause-icon.png";

                    await System.Windows.Threading.Dispatcher.Yield(System.Windows.Threading.DispatcherPriority.Render);

                    RecordingPerfProbe.Mark("recording-resume-dispatch");
                    await _rec.ResumeAsync();
                    RestoreDrawAreaOverlayIfNeeded();
                    _recordingUiClock.Start();
                    _recTimer.Start();
                    _isPaused = false;

                    RecordingPerfProbe.Mark("recording-resumed");
                }
            }
            catch (Exception ex)
            {
                RecordingPerfProbe.Mark("recording-pause-resume-error", ex.Message);

                System.Windows.MessageBox.Show(
                    $"Pause/Resume error:\n{ex.Message}",
                    "Recording",
                    MessageBoxButton.OK,
                    MessageBoxImage.Error);
            }
            finally
            {
                _pauseBusy = false;
            }
        }

        private async void BtnStop_Click()
        {
            if (_stopStarted || !_isRecording || _rec == null)
                return;

            _stopStarted = true;
            _isSavingRecording = true;
            IsEditEnabled = false;
            _recTimer.Stop();
            _recordingUiClock.Stop();
            _elapsed = _recordingUiClock.Elapsed;
            TextRecTimer = _elapsed.ToString(@"hh\:mm\:ss");
            ClearDrawAreaOverlay();

            _uiPreviewSuppressedDuringRecording = true;

            RecordingPerfProbe.Mark("recording-stop-requested");

            string deliver = null;
            string outDir = OutDirFolder;
            var rec = _rec;
            _rec = null;

            try
            {
                DebugLog.Write("[VM] BtnStop_Click -> immediate recorder stop begin");
                await rec.StopAsync();
                DebugLog.Write("[VM] BtnStop_Click -> immediate recorder stop end");
            }
            catch (Exception ex)
            {
                DebugLog.Write("[VM] BtnStop_Click -> immediate recorder stop ERROR:\n" + ex);
            }

            bool canNavigateToHome = false;
            bool closeRequestedWhileSaving = false;
            bool recordingWindowWasHidden = false;

            SavingRecordingWindowVM savingVm = null;
            NessStudio.View.SavingRecordingScreen.SavingRecordingWindow savingWindow = null;

            try
            {
                DebugLog.Write("[SAVEUI] stop requested");

                savingVm = new SavingRecordingWindowVM();
                savingVm.ApplyState(new RecordingSaveProgress
                {
                    Title = "Saving Recording...",
                    Message = "Preparing finalization...",
                    Percent = 0,
                    CurrentStep = 0,
                    TotalSteps = 8,
                    IsIndeterminate = false
                });

                savingWindow = new NessStudio.View.SavingRecordingScreen.SavingRecordingWindow
                {
                    WindowStartupLocation = WindowStartupLocation.CenterScreen,
                    ShowInTaskbar = true,
                    Topmost = true,
                    DataContext = savingVm
                };

                savingWindow.CloseRequestedWhileBusy += (s, e) =>
                {
                    closeRequestedWhileSaving = true;
                    DebugLog.Write("[SAVEUI] user requested app close while saving");
                    try
                    {
                        savingWindow.Hide();
                    }
                    catch
                    {
                    }
                };

                try
                {
                    RecordingScreenWindow.IsEnabled = false;
                    RecordingScreenWindow.Hide();
                    recordingWindowWasHidden = true;
                    DebugLog.Write("[SAVEUI] recording window hidden while saving window is visible");
                }
                catch (Exception ex)
                {
                    DebugLog.Write("[SAVEUI] failed to hide recording window:\n" + ex);
                }

                savingWindow.Show();
                DebugLog.Write("[SAVEUI] window opened");

                await System.Windows.Threading.Dispatcher.Yield(System.Windows.Threading.DispatcherPriority.Render);

                IProgress<RecordingSaveProgress> progress = new Progress<RecordingSaveProgress>(state =>
                {
                    try
                    {
                        savingVm?.ApplyState(state);
                    }
                    catch
                    {
                    }

                    DebugLog.Write($"[SAVEUI] progress {state.Percent}% | {state.Message}");
                });

                DebugLog.Write("[VM] BtnStop_Click begin");

                deliver = await Task.Run(async () =>
                {
                    return await rec.StopAndFinalizeAsync(progress);
                });

                DebugLog.Write($"[VM] BtnStop_Click deliver => {deliver}");

                if (!string.IsNullOrWhiteSpace(outDir))
                {
                    try
                    {
                        progress.Report(new RecordingSaveProgress
                        {
                            Title = "Saving Recording...",
                            Message = "Cleaning temporary artifacts...",
                            Percent = 88,
                            CurrentStep = 7,
                            TotalSteps = 8,
                            IsIndeterminate = false
                        });

                        FileCleanup.DeleteTxtAndLogArtifactsFromDeliver(outDir, recurse: false);
                        DebugLog.Write("[VM] FileCleanup end");
                    }
                    catch (Exception ex)
                    {
                        DebugLog.Write("[VM] FileCleanup warning:\n" + ex);
                    }

                    try
                    {
                        DebugLog.Write("[VM] ProjectIngestService begin (foreground)");

                        await NessStudio.ViewModel.Helpers.ProjectIngestService
                            .ProcessAsync(outDir, progress)
                            .WaitAsync(TimeSpan.FromSeconds(10));

                        DebugLog.Write("[VM] ProjectIngestService end (foreground)");
                    }
                    catch (TimeoutException)
                    {
                        DebugLog.Write("[VM] ProjectIngestService TIMEOUT (10s)");
                    }
                    catch (Exception ingestEx)
                    {
                        DebugLog.Write("[VM] ProjectIngestService ERROR:\n" + ingestEx);
                    }
                }
                else
                {
                    progress.Report(new RecordingSaveProgress
                    {
                        Title = "Saving Recording...",
                        Message = "Finishing...",
                        Percent = 100,
                        CurrentStep = 8,
                        TotalSteps = 8,
                        IsIndeterminate = false
                    });
                }

                if (string.IsNullOrWhiteSpace(deliver))
                {
                    System.Windows.MessageBox.Show(
                        "No valid audio/video tracks were generated.",
                        "Warning",
                        MessageBoxButton.OK,
                        MessageBoxImage.Warning
                    );
                }

                canNavigateToHome = true;
                DebugLog.Write("[VM] BtnStop_Click save pipeline completed");
                RecordingPerfProbe.Mark("recording-save-pipeline-completed");
            }
            catch (Exception ex)
            {
                DebugLog.Write("[VM] BtnStop_Click ERROR:\n" + ex);

                System.Windows.MessageBox.Show(
                    $"Stop error:\n{ex.Message}\n\nLog file:\n{DebugLog.GetPath()}",
                    "Recording",
                    MessageBoxButton.OK,
                    MessageBoxImage.Error);

                try
                {
                    if (savingWindow != null)
                    {
                        savingWindow.Dispatcher.Invoke(() =>
                        {
                            try
                            {
                                savingWindow.AllowClose = true;
                                savingWindow.Close();
                            }
                            catch
                            {
                            }
                        });
                    }
                }
                catch
                {
                }

                try
                {
                    RecordingScreenWindow?.Dispatcher?.Invoke(() =>
                    {
                        try
                        {
                            if (RecordingScreenWindow != null)
                            {
                                RecordingScreenWindow.Show();
                                RecordingScreenWindow.Activate();
                                RecordingScreenWindow.Focus();
                            }
                        }
                        catch
                        {
                        }
                    });
                }
                catch
                {
                }

                DebugLog.Write("[VM] Saving window closed from error path");
            }
            finally
            {
                _isRecording = false;
                _isPaused = false;
                IsEditEnabled = true;

                try
                {
                    RecordingScreenWindow.IsEnabled = true;
                }
                catch
                {
                }

                try
                {
                    if (savingWindow != null)
                    {
                        DebugLog.Write("[SAVEUI] window closing");
                        savingWindow.AllowClose = true;
                        savingWindow.Close();
                        DebugLog.Write("[SAVEUI] window closed");
                    }
                }
                catch (Exception ex)
                {
                    DebugLog.Write("[SAVEUI] window closing ERROR:\n" + ex);
                }

                _isSavingRecording = false;
                _stopStarted = false;

                ResetUiPreviewSuppression();

                try
                {
                    _ = Task.Run(() =>
                    {
                        try
                        {
                            DebugLog.Write("[VM] rec.Dispose begin (background)");
                            rec?.Dispose();
                            DebugLog.Write("[VM] rec.Dispose end (background)");
                        }
                        catch (Exception ex)
                        {
                            DebugLog.Write("[VM] rec.Dispose ERROR (background):\n" + ex);
                        }
                    });
                }
                catch (Exception ex)
                {
                    DebugLog.Write("[VM] rec.Dispose schedule ERROR:\n" + ex);
                }
            }

            DebugLog.Write($"[VM] BtnStop_Click post-finally | canNavigateToHome={canNavigateToHome} | closeRequestedWhileSaving={closeRequestedWhileSaving}");

            if (!canNavigateToHome)
            {
                DebugLog.Write("[SAVEUI] navigation skipped due to stop failure");

                if (!closeRequestedWhileSaving && recordingWindowWasHidden)
                {
                    try
                    {
                        RecordingScreenWindow.Show();
                        RecordingScreenWindow.Activate();
                        RecordingScreenWindow.Focus();
                        DebugLog.Write("[SAVEUI] recording window restored after save failure");
                    }
                    catch (Exception ex)
                    {
                        DebugLog.Write("[SAVEUI] failed to restore recording window:\n" + ex);
                    }
                }
                else if (closeRequestedWhileSaving)
                {
                    try
                    {
                        _isClosing = true;
                        RecordingScreenWindow.Close();
                    }
                    catch (Exception ex)
                    {
                        DebugLog.Write("[SAVEUI] failed to close recording window after user exit request:\n" + ex);
                    }
                }

                return;
            }

            if (closeRequestedWhileSaving)
            {
                DebugLog.Write("[SAVEUI] save completed and app close was requested by user");

                try
                {
                    _isClosing = true;
                    RecordingScreenWindow.Close();
                }
                catch (Exception ex)
                {
                    DebugLog.Write("[SAVEUI] failed to close recording window after save completed:\n" + ex);
                }

                return;
            }

            try
            {
                var home = new HomeScreenWindow
                {
                    WindowStartupLocation = WindowStartupLocation.CenterScreen
                };

                home.Show();
                System.Windows.Application.Current.MainWindow = home;

                DebugLog.Write("[SAVEUI] navigation -> Home");
                DebugLog.Write("[VM] BtnStop_Click navigation => HomeScreenWindow opened");
                RecordingPerfProbe.Mark("recording-navigation-home-opened");

                RecordingScreenWindow.Close();
            }
            catch (Exception ex)
            {
                DebugLog.Write("[VM] BtnStop_Click navigation ERROR:\n" + ex);

                try
                {
                    RecordingScreenWindow.Close();
                }
                catch
                {
                }
            }
        }

        [StructLayout(LayoutKind.Sequential, CharSet = CharSet.Auto)]
        private struct DISPLAY_DEVICE
        {
            public int cb;
            [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 32)]
            public string DeviceName;
            [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 128)]
            public string DeviceString;
            public int StateFlags;
            [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 128)]
            public string DeviceID;
            [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 128)]
            public string DeviceKey;
        }
        [DllImport("user32.dll", CharSet = CharSet.Auto)]
        private static extern bool EnumDisplayDevices(string lpDevice, int iDevNum, ref DISPLAY_DEVICE lpDisplayDevice, int dwFlags);
        private static string GetDisplayFriendlyName(string deviceName)
        {
            var dd = new DISPLAY_DEVICE { cb = Marshal.SizeOf<DISPLAY_DEVICE>() };
            int i = 0;
            while (EnumDisplayDevices(null, i, ref dd, 0))
            {
                if (string.Equals(dd.DeviceName, deviceName, StringComparison.OrdinalIgnoreCase))
                {
                    var ddMon = new DISPLAY_DEVICE { cb = Marshal.SizeOf<DISPLAY_DEVICE>() };
                    if (EnumDisplayDevices(dd.DeviceName, 0, ref ddMon, 0) && !string.IsNullOrWhiteSpace(ddMon.DeviceString))
                        return ddMon.DeviceString;
                    return string.IsNullOrWhiteSpace(dd.DeviceString) ? deviceName : dd.DeviceString;
                }
                dd = new DISPLAY_DEVICE { cb = Marshal.SizeOf<DISPLAY_DEVICE>() };
                i++;
            }
            return deviceName;
        }
        public void StartDrawAreaSelection()
        {
            var ghostOwner = new System.Windows.Window
            {
                Width = 1,
                Height = 1,
                Left = 0,
                Top = 0,
                WindowStyle = System.Windows.WindowStyle.None,
                AllowsTransparency = true,
                ShowInTaskbar = false,
                Opacity = 0,
                ShowActivated = false
            };
            ghostOwner.Show();
            try
            {
                var win = new DrawAreaScreenWindow
                {
                    Owner = ghostOwner,
                    ShowInTaskbar = false,
                    WindowStartupLocation = WindowStartupLocation.Manual
                };
                bool? ok = win.ShowDialog();
                if (ok == true && win.Result.HasValue)
                {
                    _lastDrawArea = win.Result.Value;
                    int w = ((int)Math.Round(_lastDrawArea.Value.Width)) & ~1;
                    int h = ((int)Math.Round(_lastDrawArea.Value.Height)) & ~1;
                    DrawAreaInfoText = $"Draw Area: {w} x {h}";
                    IsDrawAreaInfoVisible = true;
                    _lastDrawArea = new System.Windows.Rect(_lastDrawArea.Value.X, _lastDrawArea.Value.Y, w, h);
                    var drawItem = ScreenOptions.FirstOrDefault(o => o.Value == null &&
                                 string.Equals(o.Display, DrawAreaLabel, StringComparison.OrdinalIgnoreCase));
                    if (drawItem != null)
                    {
                        _suppressSelectionHandler = true;
                        SelectedScreenOption = drawItem;
                        _suppressSelectionHandler = false;
                    }
                    SelectedScreen = null;
                    StopScreenPreview();
                    ScreenPreviewImage = null;
                }
                else
                {
                    _lastDrawArea = null;
                    DrawAreaInfoText = string.Empty;
                    IsDrawAreaInfoVisible = false;
                    IsScreenEnabled = false;
                    PopulateScreens();
                }
            }
            finally
            {
                ghostOwner.Close();
            }
        }
        private void SelectPrimaryScreen()
        {
            var primary = ScreenOptions?.FirstOrDefault(o => o?.Value?.Primary == true && o.Value != null)
            ?? ScreenOptions?.FirstOrDefault(o => o?.Value != null);
            if (primary != null)
                SelectedScreenOption = primary;
        }
    }
}

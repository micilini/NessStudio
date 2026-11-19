using NAudio.CoreAudioApi;
using NessStudio.Components.Header;
using NessStudio.Components.Menu;
using NessStudio.Components.RecentProjects;
using NessStudio.Models;
using NessStudio.Recording.Engines;
using NessStudio.Recording;
using NessStudio.Presentation;
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
        private readonly NativeRecordingWorkspace _previews;
        private readonly IRecorderEngineFactory _recorderFactory;
        private readonly RecordingPreferencesService _preferences;
        public RecordingState CurrentRecordingState { get; private set; } = RecordingState.Created;
        private bool _uiPreviewSuppressedDuringRecording = false;
        private bool _pauseBusy;
        private bool _startBusy;
        private bool _allowClose;
        private bool _exitAfterSave;
        private Task? _startTask;
        private Task? _stopTask;
        private RecordingStatistics _lastStatistics = new(null, null, null);
        private RecordingSelection? _lastSelection;
        /// <summary>Gets whether a new recording can be admitted by the frontend.</summary>
        public bool CanStartRecording => _isLoaded && !_startBusy && !_isRecording && !_stopStarted && !_isSavingRecording && !_isClosing;
        /// <summary>Gets whether a pause/resume command is compatible with the observed engine state.</summary>
        public bool CanPauseRecording => _isRecording && !_startBusy && !_pauseBusy && !_stopStarted && !_isClosing &&
            CurrentRecordingState is RecordingState.Recording or RecordingState.Paused;
        /// <summary>Gets whether a Stop can be admitted; a concurrent pause is serialized by the service.</summary>
        public bool CanStopRecording => _isRecording && !_startBusy && !_stopStarted && !_isSavingRecording && !_isClosing;
        /// <summary>Gets copied timing/statistics from the selected engine.</summary>
        public RecordingStatistics RecordingStatistics => _rec?.Statistics ?? _lastStatistics;
        /// <summary>Gets the actual approved native configuration, when available.</summary>
        public RecordingSelection? EffectiveRecordingSelection => _rec?.Selection ?? _lastSelection;
        /// <summary>Gets the latest recording or persistence failure.</summary>
        public string? RecordingError { get; private set; }
        private void RefreshRecordingCommands()
        {
            OnPropertyChanged(nameof(CanStartRecording)); OnPropertyChanged(nameof(CanPauseRecording)); OnPropertyChanged(nameof(CanStopRecording));
            OnPropertyChanged(nameof(CanEditMicrophoneLevel)); OnPropertyChanged(nameof(CanEditSystemAudioLevel));
            CommandManager.InvalidateRequerySuggested();
        }
        private void RefreshEngineTime()
        {
            if (RecordingStatistics.ActiveDuration is { } duration) _elapsed = duration;
            TextRecTimer = _elapsed.ToString(@"hh\:mm\:ss");
            OnPropertyChanged(nameof(RecordingStatistics));
        }
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
        private double _microphoneLevel = 100, _systemAudioLevel = 100;
        public double MicrophoneLevel
        {
            get => _microphoneLevel;
            set { _microphoneLevel = RecordingPreferencesService.NormalizeLevel(value); OnPropertyChanged(nameof(MicrophoneLevel)); }
        }
        public double SystemAudioLevel
        {
            get => _systemAudioLevel;
            set { _systemAudioLevel = RecordingPreferencesService.NormalizeLevel(value); OnPropertyChanged(nameof(SystemAudioLevel)); }
        }
        public bool CanEditMicrophoneLevel => CanStartRecording && IsEditEnabled && IsMicrophoneEnabled;
        public bool CanEditSystemAudioLevel => CanStartRecording && IsEditEnabled && IsSystemAudioEnabled;
        public bool IsMicrophoneEnabled
        {
            get => _isMicrophoneEnabled;
            set
            {
                if (_isMicrophoneEnabled != value)
                {
                    _isMicrophoneEnabled = value;
                    OnPropertyChanged(nameof(IsMicrophoneEnabled));
                    OnPropertyChanged(nameof(CanEditMicrophoneLevel));
                    MicrophoneIcon = _isMicrophoneEnabled
                        ? "/Assets/Images/microfone-icon.png"
                        : "/Assets/Images/microfone-closed-icon.png";
                    UpdateAudioMonitoring();
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
                    OnPropertyChanged(nameof(CanEditSystemAudioLevel));
                    SystemAudioIcon = _isSystemAudioEnabled
                        ? "/Assets/Images/system-audio-icon.png"
                        : "/Assets/Images/system-audio-closed-icon.png";
                    UpdateAudioMonitoring();
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
                    OnPropertyChanged(nameof(CanEditMicrophoneLevel));
                    OnPropertyChanged(nameof(CanEditSystemAudioLevel));
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
        public double RecorderWindowHeight => IsRecordingPanelVisible ? 221 : 316;
        public bool IsRecordingPanelVisible
        {
            get => isRecordingPanelVisible;
            set
            {
                isRecordingPanelVisible = value;
                OnPropertyChanged("IsRecordingPanelVisible");
                OnPropertyChanged(nameof(RecorderWindowHeight));
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
                OnPropertyChanged(nameof(FooterStatusMessage));
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
        private RecordingExecutionMode _selectedExecutionMode;
        /// <summary>Gets or sets the requested recording policy before a session is created.</summary>
        public RecordingExecutionMode SelectedExecutionMode
        {
            get => _selectedExecutionMode;
            set
            {
                if (!Enum.IsDefined(value) || _startBusy || _isRecording || _isSavingRecording || _selectedExecutionMode == value) return;
                _selectedExecutionMode = value;
                System.Windows.Media.RenderOptions.ProcessRenderMode = value == RecordingExecutionMode.FullSoftware
                    ? System.Windows.Interop.RenderMode.SoftwareOnly : ((App)System.Windows.Application.Current).Options.PresentationMode;
                if (_isLoaded) UpdateIdlePreview();
                OnPropertyChanged(nameof(SelectedExecutionMode)); OnPropertyChanged(nameof(CurrentSettingsSummary));
                OnPropertyChanged(nameof(FooterStatusMessage));
            }
        }
        public string CurrentSettingsSummary => $"{SelectedRecordingFps} fps • {SelectedRecordingTimerSeconds}s • {SelectedExecutionMode switch { RecordingExecutionMode.FullSoftware => "CPU", RecordingExecutionMode.RequireGpuResidentHardware => "GPU", RecordingExecutionMode.GpuWithSoftwareEncoding => "GPU + CPU", _ => "Auto" }}";
        public string FooterStatusMessage => $"Recording mode: {SelectedExecutionMode switch { RecordingExecutionMode.FullSoftware => "CPU", RecordingExecutionMode.RequireGpuResidentHardware => "GPU", RecordingExecutionMode.GpuWithSoftwareEncoding => "GPU + CPU", _ => "Automatic" }} • {SelectedRecordingFps} FPS • {DiskSpaceMessage}";
        public ObservableCollection<RecordingPresetOption> FpsOptions { get; } = new();
        public ObservableCollection<RecordingPresetOption> TimerOptions { get; } = new();
        public ObservableCollection<RecordingPresetOption> ExecutionModeOptions { get; } = new();
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
        public RecordingCameraDescriptor SelectedWebcam
        {
            get => _selectedWebcam;
            set
            {
                _selectedWebcam = value; OnPropertyChanged(nameof(SelectedWebcam)); CameraFormats.Clear();
                if (value is not null) foreach (var format in value.Formats) CameraFormats.Add(format);
                SelectedCameraFormat = CameraFormats.FirstOrDefault(format => format.Width == 1280 && format.Height == 720 && format.FpsNum == 30 && format.FpsDen == 1) ?? CameraFormats.FirstOrDefault();
                if (_isLoaded) RestartWebcamPreview();
            }
        }
        public ObservableCollection<RecordingCameraOptions> CameraFormats { get; } = new();
        private RecordingCameraOptions? _selectedCameraFormat;
        public RecordingCameraOptions? SelectedCameraFormat { get => _selectedCameraFormat; set { _selectedCameraFormat=value; OnPropertyChanged(nameof(SelectedCameraFormat)); if (_isLoaded) UpdateWebcamPreviewRunning(); } }
        public MMDevice SelectedMicrophone { get => _selectedMicrophone; set { _selectedMicrophone = value; OnPropertyChanged(nameof(SelectedMicrophone)); UpdateAudioMonitoring(); } }
        public MMDevice SelectedRenderLoopback { get => _selectedRender; set { _selectedRender = value; OnPropertyChanged(nameof(SelectedRenderLoopback)); UpdateAudioMonitoring(); } }
        private Screen _selectedScreen;
        private RecordingCameraDescriptor _selectedWebcam;
        private MMDevice _selectedMicrophone;
        private MMDevice _selectedRender;

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
        private DispatcherTimer? _storageTimer;
        private DispatcherTimer? _audioMeterTimer;
        private readonly AudioLevelMonitor _audioMonitor = new();
        private double _microphoneSignalLevel, _systemAudioSignalLevel;
        public double MicrophoneSignalLevel
        {
            get => _microphoneSignalLevel;
            private set { if (_microphoneSignalLevel == value) return; _microphoneSignalLevel = value; OnPropertyChanged(nameof(MicrophoneSignalLevel)); }
        }
        public double SystemAudioSignalLevel
        {
            get => _systemAudioSignalLevel;
            private set { if (_systemAudioSignalLevel == value) return; _systemAudioSignalLevel = value; OnPropertyChanged(nameof(SystemAudioSignalLevel)); }
        }
        private readonly Func<string, ulong> _availableSpace;
        private RecordingStorageStatus? _storageStatus;
        private bool _storageQueryBusy, _storageQueryErrorLogged, _storageQueryUnavailable;
        private string? _storageAutoStopMessage;
        public string DiskSpaceMessage => _storageQueryUnavailable ? "Free disk space is currently unavailable."
            : _storageStatus?.Message ?? "Checking free disk space...";
        public bool IsDiskSpaceLow => !_storageQueryUnavailable && _storageStatus?.IsLow == true;
        private TimeSpan _elapsed;
        private bool _isClosing = false;
        private IRecordingService _rec;
        public ObservableCollection<System.Windows.Forms.Screen> Screens { get; } = new();
        public ObservableCollection<RecordingCameraDescriptor> Webcams { get; } = new();
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
        public DelegateCommand SelectExecutionModeCommand { get; set; }
        private const string DrawAreaLabel = "Draw Area";
        public RecordingScreenWindowVM(RecordingScreenWindow recordingScreen, IRecorderEngineFactory recorderFactory,
            RecordingPreferencesService preferences, NativeRecordingWorkspace previews, Func<string, ulong>? availableSpace = null)
        {
            RecordingScreenWindow = recordingScreen;
            _recorderFactory = recorderFactory ?? throw new ArgumentNullException(nameof(recorderFactory));
            _preferences = preferences ?? throw new ArgumentNullException(nameof(preferences));
            _previews = previews ?? throw new ArgumentNullException(nameof(previews));
            _availableSpace = availableSpace ?? RecordingStorageSpace.AvailableBytes;
            PlayRecodingCommand = new PlayRecodingCommand(this);
            PauseRecordingCommand = new PauseRecordingCommand(this);
            StopRecordingCommand = new StopRecordingCommand(this);
            ToggleSettingsMenuCommand = new DelegateCommand(_ => ToggleSettingsMenu());
            OpenFpsSubmenuCommand = new DelegateCommand(_ => OpenFpsSubmenu());
            OpenTimerSubmenuCommand = new DelegateCommand(_ => OpenTimerSubmenu());
            CloseSettingsMenusCommand = new DelegateCommand(_ => CloseSettingsMenus());
            SelectFpsOptionCommand = new DelegateCommand(param => SelectFpsOption(param));
            SelectTimerOptionCommand = new DelegateCommand(param => SelectTimerOption(param));
            SelectExecutionModeCommand = new DelegateCommand(param => SelectExecutionMode(param));
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
                if (_storageTimer is null)
                {
                    _storageTimer = new DispatcherTimer(DispatcherPriority.Background) { Interval = TimeSpan.FromSeconds(1) };
                    _storageTimer.Tick += async (_, _) => await RefreshDiskSpaceAsync();
                }
                _storageTimer.Start();
                _ = RefreshDiskSpaceAsync();

                if (_recTimer == null)
                {
                    _recTimer = new DispatcherTimer(DispatcherPriority.Background);
                    _recTimer.Interval = TimeSpan.FromMilliseconds(250);
                    _recTimer.Tick += (s2, e2) =>
                    {
                        RefreshEngineTime();
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

                await PopulateWebcamsAsync();
                DebugLog.Write("[VM] RecordingScreenWindow_Loaded -> PopulateWebcams end");
                RecordingPerfProbe.Mark("recording-window-webcams-populated", $"webcams={Webcams.Count}");

                await RecordingScreenWindow.Dispatcher.InvokeAsync(
                    () => { },
                    DispatcherPriority.Background);

                PopulateMicrophonesAndSystem();
                DebugLog.Write("[VM] RecordingScreenWindow_Loaded -> PopulateMicrophonesAndSystem end");
                RecordingPerfProbe.Mark("recording-window-audio-populated", $"mics={Microphones.Count}");

                _isLoaded = true;
                _audioMeterTimer ??= new DispatcherTimer(TimeSpan.FromMilliseconds(50), DispatcherPriority.Background,
                    (_, _) => RefreshAudioMeters(), RecordingScreenWindow.Dispatcher);
                UpdateAudioMonitoring();
                _audioMeterTimer.Start();
                RefreshRecordingCommands();

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
                DebugLog.Write("[VM] RecordingScreenWindow_Loaded ERROR:\n" + UserError.Details(ex));
                RecordingPerfProbe.Mark("recording-window-loaded-error", UserError.Message(ex));

                MessageBox.Show(
                    $"Initialization error: {UserError.Message(ex)}",
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
            ExecutionModeOptions.Clear();
            ExecutionModeOptions.Add(new RecordingPresetOption { Label = "Automatic", Value = (int)RecordingExecutionMode.Automatic });
            ExecutionModeOptions.Add(new RecordingPresetOption { Label = "GPU (required)", Value = (int)RecordingExecutionMode.RequireGpuResidentHardware });
            ExecutionModeOptions.Add(new RecordingPresetOption { Label = "CPU (FullSoftware)", Value = (int)RecordingExecutionMode.FullSoftware });
            ExecutionModeOptions.Add(new RecordingPresetOption { Label = "GPU + CPU (x264)", Value = (int)RecordingExecutionMode.GpuWithSoftwareEncoding });
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
            var levels = _preferences.Load();
            MicrophoneLevel = levels.MicrophoneLevel;
            SystemAudioLevel = levels.SystemAudioLevel;
            SelectedRecordingTimerSeconds = RecordingPreferencesService.NormalizeTimer(app.RecordingTimerSeconds);
            SelectedRecordingFps = RecordingPreferencesService.NormalizeFps(app.RecordingFps);
            SelectedExecutionMode = app.RecordingExecutionMode;
        }
        private void RefreshRecordingPresetSelections()
        {
            foreach (var option in FpsOptions)
                option.IsSelected = option.Value == SelectedRecordingFps;
            foreach (var option in TimerOptions)
                option.IsSelected = option.Value == SelectedRecordingTimerSeconds;
            foreach (var option in ExecutionModeOptions)
                option.IsSelected = option.Value == (int)SelectedExecutionMode;
            OnPropertyChanged(nameof(CurrentSettingsSummary));
        }
        private bool CanChangeRecordingPreferences()
        {
            return IsEditEnabled && CanStartRecording;
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
            var service = _preferences;
            var current = service.Load();
            current.RecordingFps = SelectedRecordingFps;
            current.TimerSeconds = SelectedRecordingTimerSeconds;
            current.ExecutionMode = SelectedExecutionMode;
            current.MicrophoneLevel = MicrophoneLevel;
            current.SystemAudioLevel = SystemAudioLevel;
            service.Save(current);
        }
        private void SelectExecutionMode(object parameter)
        {
            if (!CanChangeRecordingPreferences() || parameter is not int mode || !Enum.IsDefined((RecordingExecutionMode)mode)) return;
            SelectedExecutionMode = (RecordingExecutionMode)mode;
            ((App)System.Windows.Application.Current).RecordingExecutionMode = SelectedExecutionMode;
            SaveRecordingPreferencesToDisk(); RefreshRecordingPresetSelections(); CloseSettingsMenus();
        }
        private RecordingRuntimeOptions BuildRuntimeOptions()
        {
            return new RecordingRuntimeOptions
            {
                RecordingFps = SelectedRecordingFps,
                CountdownSeconds = SelectedRecordingTimerSeconds
            };
        }
        private async void RecordingScreenWindow_Unloaded(object sender, RoutedEventArgs e)
        {
            _audioMeterTimer?.Stop();
            _storageTimer?.Stop();
            await _audioMonitor.DisposeAsync();
            StopScreenPreview();
            StopWebcamPreview();

            try
            {
                await _previews.DisposeAsync();
            }
            catch
            {
            }
        }
        private void RecordingScreenWindow_Closing(object sender, CancelEventArgs e)
        {
            if (_allowClose) return;
            e.Cancel = true;
            if (_isClosing) return;
            if (_stopStarted || _isSavingRecording)
            {
                _exitAfterSave = true;
                DebugLog.Write("[SAVEUI] close queued until finalization and project persistence complete");
                return;
            }
            if (_isRecording && MessageBox.Show(
                "A recording is in progress.\nDo you want to stop, save and exit?", "Stop recording and exit?",
                MessageBoxButton.YesNo, MessageBoxImage.Question, MessageBoxResult.No) != MessageBoxResult.Yes) return;
            _isClosing = true;
            _exitAfterSave = true;
            IsEditEnabled = false;
            RefreshRecordingCommands();
            _ = CloseRecordingWindowAsync();
        }
        private async Task CloseRecordingWindowAsync()
        {
            try
            {
                if (_startTask is { } starting) await starting;
                if (_isRecording && _rec is not null)
                {
                    _stopTask ??= StopRecordingCoreAsync();
                    await _stopTask;
                    return;
                }
                if (_rec is { } rec)
                {
                    rec.StateChanged -= RecordingService_StateChanged;
                    await rec.DisposeAsync();
                    _rec = null;
                }
                await CloseAfterCleanupAsync();
            }
            catch (Exception error)
            {
                DebugLog.Write("[RecordingService] Close: " + UserError.Details(error));
                RecordingError = UserError.Message(error);
                OnPropertyChanged(nameof(RecordingError));
                _isClosing = false;
                IsEditEnabled = true;
            }
        }
        private async Task CloseAfterCleanupAsync()
        {
            _isClosing = true;
            _storageTimer?.Stop();
            _audioMeterTimer?.Stop();
            await _audioMonitor.DisposeAsync();
            ClearDrawAreaOverlay();
            _recTimer?.Stop();
            await _previews.DisposeAsync();
            _allowClose = true;
            // A workspace with no idle owner can dispose synchronously inside Closing.
            // Let that event unwind before submitting the final close to WPF.
            await System.Windows.Threading.Dispatcher.Yield(DispatcherPriority.Background);
            RecordingScreenWindow.Close();
        }

        private void ApplyDiskSpace(RecordingStorageStatus status)
        {
            _storageQueryUnavailable = false;
            _storageStatus = status;
            OnPropertyChanged(nameof(DiskSpaceMessage));
            OnPropertyChanged(nameof(FooterStatusMessage));
            OnPropertyChanged(nameof(IsDiskSpaceLow));
        }

        private async Task RefreshDiskSpaceAsync()
        {
            if (_storageQueryBusy || _isClosing) return;
            _storageQueryBusy = true;
            try
            {
                string directory = OutDirFolder ?? ((App)System.Windows.Application.Current).Options.RecordingsDirectory;
                var status = new RecordingStorageStatus(await Task.Run(() => _availableSpace(directory)));
                if (_isClosing) return;
                ApplyDiskSpace(status);
                // Use the ordinary Stop/Save path once, including while paused.
                // The space query never runs on the capture or presentation thread.
                if (status.MustStop && _isRecording && !_startBusy && !_stopStarted && _rec is not null)
                {
                    _storageAutoStopMessage = $"Recording stopped automatically because only {status.FreeSpace} of free disk space remained (limit: 50 MiB).";
                    DebugLog.WriteError("[RecordingStorage] " + _storageAutoStopMessage);
                    BtnStop_Click();
                }
            }
            catch (Exception error)
            {
                _storageQueryUnavailable = true;
                if (!_isClosing) { OnPropertyChanged(nameof(DiskSpaceMessage)); OnPropertyChanged(nameof(FooterStatusMessage)); OnPropertyChanged(nameof(IsDiskSpaceLow)); }
                if (!_storageQueryErrorLogged)
                {
                    _storageQueryErrorLogged = true;
                    DebugLog.WriteError("[RecordingStorage] Space query failed: " + UserError.Details(error));
                }
                // An unavailable query is not evidence that the disk is full.
            }
            finally { _storageQueryBusy = false; }
        }

        private void UpdateAudioMonitoring()
        {
            if (!_isLoaded || _isClosing) return;
            _audioMonitor.Configure(IsMicrophoneEnabled ? SelectedMicrophone?.ID : null,
                IsSystemAudioEnabled ? SelectedRenderLoopback?.ID : null);
            if (!IsMicrophoneEnabled || SelectedMicrophone is null) MicrophoneSignalLevel = 0;
            if (!IsSystemAudioEnabled || SelectedRenderLoopback is null) SystemAudioSignalLevel = 0;
        }

        private void RefreshAudioMeters()
        {
            if (_isClosing) return;
            var levels = _audioMonitor.ReadLevels();
            MicrophoneSignalLevel = IsMicrophoneEnabled ? ReleaseMeter(MicrophoneSignalLevel, levels.Microphone) : 0;
            SystemAudioSignalLevel = IsSystemAudioEnabled ? ReleaseMeter(SystemAudioSignalLevel, levels.SystemAudio) : 0;
        }

        private static double ReleaseMeter(double previous, double current)
        {
            double level = Math.Max(Math.Clamp(current, 0, 100), previous * 0.72);
            return level < 0.1 ? 0 : level;
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
                string friendly = NessStudio.Interop.Legacy.MonitorNames.GetFriendlyName(scr.DeviceName);
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
        private async Task PopulateWebcamsAsync()
        {
            Webcams.Clear();
            foreach (var camera in await _previews.GetCamerasAsync()) Webcams.Add(camera);
            SelectedWebcam = Webcams.FirstOrDefault();
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
        private void UpdateIdlePreview() => _ = UpdateIdlePreviewAsync();
        private async Task UpdateIdlePreviewAsync()
        {
            if (!_isLoaded || _uiPreviewSuppressedDuringRecording || _isClosing || _startBusy || _isRecording) return;
            try
            {
                await _previews.UpdateIdleAsync(IsScreenEnabled ? LegacyGeometry.Describe(SelectedScreen) : null,
                    IsWebcamEnabled ? SelectedCameraFormat : null, SelectedExecutionMode,
                    IsDrawAreaInfoVisible ? LegacyGeometry.Describe(_lastDrawArea) : null);
            }
            catch (Exception error) { DebugLog.Write("[NativePreview] " + UserError.Details(error)); }
        }
        private void UpdateScreenPreviewRunning() => UpdateIdlePreview();
        private void StopScreenPreview() { }
        private void RestartScreenPreview() => UpdateIdlePreview();
        public void CaptureScreenFrame() => UpdateIdlePreview();
        private void RecordingService_StateChanged(object? sender, RecordingStateChangedEventArgs change)
        {
            if (!ReferenceEquals(sender, _rec)) return;
            var dispatcher = RecordingScreenWindow.Dispatcher;
            if (dispatcher.HasShutdownStarted) return;
            if (!dispatcher.CheckAccess())
            {
                dispatcher.BeginInvoke(() => RecordingService_StateChanged(sender, change));
                return;
            }
            if (change.State == RecordingState.Disposed) return;
            // The native assets may be complete while the project still awaits persistence.
            CurrentRecordingState = change.State == RecordingState.Completed && _isSavingRecording
                ? RecordingState.Finalizing : change.State;
            OnPropertyChanged(nameof(CurrentRecordingState));
            OnPropertyChanged(nameof(EffectiveRecordingSelection));
            RefreshEngineTime(); RefreshRecordingCommands();
            if (change.State == RecordingState.Failed)
            {
                RecordingError = change.Error is null ? "The recording engine failed." : UserError.Message(change.Error);
                if (_isRecording && !_startBusy && !_stopStarted)
                    DebugLog.WriteError("[RecordingService] Recording failed: " +
                        (change.Error is null ? RecordingError : UserError.Details(change.Error)));
                OnPropertyChanged(nameof(RecordingError));
                _recTimer?.Stop();
                if (_isRecording && !_startBusy && !_stopStarted) BtnStop_Click();
            }
        }

        private void UpdateWebcamPreviewRunning() => UpdateIdlePreview();
        private void StopWebcamPreview() { }
        private void RestartWebcamPreview() => UpdateIdlePreview();
        private Task DisableUiPreviewsForActiveRecordingAsync()
        {
            _uiPreviewSuppressedDuringRecording = true;
            return Task.CompletedTask;
        }
        private void ResetUiPreviewSuppression()
        {
            _uiPreviewSuppressedDuringRecording = false;
            _previews.ResumeIdle();
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
                DebugLog.Write("[VM] draw area overlay show ERROR:\n" + UserError.Details(ex));
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
                DebugLog.Write("[VM] draw area overlay close ERROR:\n" + UserError.Details(ex));
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

        private void BtnRec_Click()
        {
            if (!CanStartRecording) return;
            _startBusy = true;
            RecordingError = null;
            _lastStatistics = new(null, null, null); _lastSelection = null;
            RefreshRecordingCommands();
            _startTask = StartRecordingAsync();
        }
        private async Task StartRecordingAsync()
        {
            try { await StartRecordingCoreAsync(); }
            finally
            {
                _startBusy = false;
                RefreshRecordingCommands();
                if (!_isRecording && !_isClosing) UpdateIdlePreview();
                if (_isRecording && _rec?.State == RecordingState.Failed && !_stopStarted) BtnStop_Click();
            }
        }
        private async Task StartRecordingCoreAsync()
        {

            bool isDrawArea =
                SelectedScreenOption != null &&
                SelectedScreenOption.Value == null &&
                string.Equals(SelectedScreenOption.Display, DrawAreaLabel, StringComparison.OrdinalIgnoreCase);

            if (isDrawArea && !_lastDrawArea.HasValue)
            {
                MessageBox.Show(
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

                MessageBox.Show(
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

                MessageBox.Show(
                    "Could not resolve a valid screen target. Please select a screen or a draw area again.",
                    "Recording",
                    MessageBoxButton.OK,
                    MessageBoxImage.Warning
                );

                return;
            }

            string outDir = Path.Combine(
                ((App)System.Windows.Application.Current).Options.RecordingsDirectory,
                DateTime.Now.ToString("yyyyMMdd_HHmmss_fff") + "_" + Guid.NewGuid().ToString("N")[..6]);

            try
            {
                Directory.CreateDirectory(outDir);
                _storageAutoStopMessage = null;
                ApplyDiskSpace(await Task.Run(() => RecordingStorageSpace.EnsureCanStart(outDir, _availableSpace)));
                // Verify storage before opening devices or starting the countdown.
                using (var probe = new FileStream(Path.Combine(outDir, ".write-check"), FileMode.CreateNew,
                    FileAccess.Write, FileShare.None, 1, FileOptions.DeleteOnClose))
                {
                    probe.WriteByte(0);
                    probe.Flush(flushToDisk: true);
                }
                OutDirFolder = outDir;
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
                    displayFriendlyName: (screen != null) ? NessStudio.Interop.Legacy.MonitorNames.GetFriendlyName(screen.DeviceName) : null
                );

                if (!selection.AnyAudio && !selection.AnyVideo && !IsScreenEnabled && !isDrawArea)
                {
                    IsCountdownVisible = false;
                    IsRecButtonVisible = true;
                    IsEditEnabled = true;

                    MessageBox.Show(
                        "Select at least one video or audio source.",
                        "Recording",
                        MessageBoxButton.OK,
                        MessageBoxImage.Warning
                    );
                    return;
                }

                var targets = selection.BuildTargets(LegacyGeometry.Describe(IsScreenEnabled || isDrawArea ? screen : null));

                DebugLog.Write($"[VM] Creating recorder | outDir={outDir} | crop={(crop.HasValue ? crop.Value.ToString() : "null")}");
                IsEditEnabled = false;
                await Task.Run(SaveRecordingPreferencesToDisk);
                await _previews.SuspendAsync();
                _rec = _recorderFactory.Create(new RecordingRequest(outDir, targets, LegacyGeometry.Describe(crop), runtimeOptions,
                    SelectedExecutionMode, Camera: IsWebcamEnabled ? SelectedCameraFormat : null)
                    { MicrophoneGain = (float)(MicrophoneLevel / 100), SystemAudioGain = (float)(SystemAudioLevel / 100) });
                _previews.Adopt(_rec);
                _rec.StateChanged += RecordingService_StateChanged;

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
                        if (_isClosing) break;
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
                if (_isClosing) return;
                RecordingPerfProbe.Mark("recording-prepare-finished");
                DebugLog.Write("[VM] _rec.PrepareAsync() succeeded");

                _isPaused = false;
                _elapsed = TimeSpan.Zero;
                TextRecTimer = "00:00:00";
                PauseResumeText = "Pause";
                IsRecordingPanelVisible = true;
                IsSettingsPanelVisible = false;

                await System.Windows.Threading.Dispatcher.Yield(System.Windows.Threading.DispatcherPriority.Render);

                DebugLog.Write("[VM] Calling _rec.StartAsync()");
                RecordingPerfProbe.Mark("recording-start-dispatch");
                await _rec.StartAsync();
                _isRecording = true;
                _recTimer.Start();
                RefreshEngineTime();
                RefreshRecordingCommands();

                DebugLog.Write("[VM] _rec.StartAsync() succeeded");
                RecordingPerfProbe.Mark("recording-start-succeeded");

                if (isDrawArea && crop.HasValue)
                    ShowDrawAreaOverlay(crop.Value);

                await DisableUiPreviewsForActiveRecordingAsync();

                IsEditEnabled = true;
                RefreshEngineTime();
                RecordingPerfProbe.Mark("recording-running");
            }
            catch (Exception ex)
            {
                RecordingError = UserError.Message(ex);
                CurrentRecordingState = RecordingState.Failed;
                OnPropertyChanged(nameof(RecordingError)); OnPropertyChanged(nameof(CurrentRecordingState));
                DebugLog.WriteError("[VM] BtnRec_Click ERROR:\n" + UserError.Details(ex));

                try { _recTimer.Stop(); } catch { }
                try { if (_rec is not null) { _rec.StateChanged -= RecordingService_StateChanged; await _rec.DisposeAsync(); } } catch (Exception cleanupError) { DebugLog.Write("[RecordingService] cleanup: " + cleanupError); }

                _rec = null;
                _isRecording = false;
                _isPaused = false;
                _elapsed = TimeSpan.Zero;

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
                        UpdateIdlePreview();
                }
                catch
                {
                }

                if (!_isClosing) MessageBox.Show(
                    $"Failed to start recording:\n{UserError.Message(ex)}\n\n{UserError.Message(ex.InnerException)}\n\nLog file:\n{DebugLog.GetErrorPath()}",
                    "Recording",
                    MessageBoxButton.OK,
                    MessageBoxImage.Error
                );
            }
        }

        private async void BtnPauseResume_Click()
        {
            if (!CanPauseRecording || _rec is null) return;
            _pauseBusy = true; RefreshRecordingCommands();
            var rec = _rec;
            try
            {
                if (!_isPaused)
                {
                    await rec.PauseAsync();
                    if (_stopStarted) return;
                    _isPaused = true;
                    PauseResumeText = "Resume";
                    ButtonPlayPauseIcon = "/Assets/Images/play-icon.png";
                    CloseDrawAreaOverlayWindow();
                    RecordingPerfProbe.Mark("recording-paused");
                }
                else
                {
                    await rec.ResumeAsync();
                    if (_stopStarted) return;
                    _isPaused = false;
                    PauseResumeText = "Pause";
                    ButtonPlayPauseIcon = "/Assets/Images/pause-icon.png";
                    RestoreDrawAreaOverlayIfNeeded();
                    RecordingPerfProbe.Mark("recording-resumed");
                }
                RefreshEngineTime();
            }
            catch (Exception error)
            {
                RecordingError = UserError.Message(error); OnPropertyChanged(nameof(RecordingError));
                DebugLog.Write("[VM] Pause/Resume: " + UserError.Details(error));
                if (!_stopStarted) BtnStop_Click();
            }
            finally
            {
                // A legacy command may complete synchronously. Keep admission
                // closed through this Dispatcher turn so duplicate input cannot
                // immediately reverse the acknowledged operation.
                await Dispatcher.Yield(DispatcherPriority.Background);
                _pauseBusy = false; RefreshRecordingCommands();
            }
        }

        private void BtnStop_Click()
        {
            if (_stopStarted || !_isRecording || _rec is null) return;
            _stopTask = StopRecordingCoreAsync();
        }
        private async Task StopRecordingCoreAsync()
        {
            if (_stopStarted || _rec is null) return;
            _stopStarted = true; _isSavingRecording = true; IsEditEnabled = false;
            _recTimer?.Stop(); RefreshEngineTime(); RefreshRecordingCommands();
            ClearDrawAreaOverlay(); _uiPreviewSuppressedDuringRecording = true;
            RecordingPerfProbe.Mark("recording-stop-requested");
            var rec = _rec;
            var savingVm = new SavingRecordingWindowVM();
            var savingWindow = new NessStudio.View.SavingRecordingScreen.SavingRecordingWindow
            {
                WindowStartupLocation = WindowStartupLocation.CenterScreen, Topmost = true,
                ShowInTaskbar = true, DataContext = savingVm
            };
            // The close request remains visible while workers and project persistence finish.
            savingWindow.CloseRequestedWhileBusy += (_, _) =>
            {
                _exitAfterSave = true;
                savingVm.ApplyState(new RecordingSaveProgress { Title = "Finishing before closing...",
                    Message = "Waiting for recording files and project to finish...", IsIndeterminate = true });
            };
            bool saved = false;
            try
            {
                savingVm.ApplyState(new RecordingSaveProgress { Title = "Saving Recording...", Message = "Finishing recording files...", IsIndeterminate = true });
                savingWindow.Show();
                RecordingScreenWindow.Hide();
                await System.Windows.Threading.Dispatcher.Yield(DispatcherPriority.Render);
                IProgress<RecordingSaveProgress> progress = new Progress<RecordingSaveProgress>(savingVm.ApplyState);
                // The service closes the captured tracks and publishes their metadata.
                string deliver = await rec.StopAndFinalizeAsync(progress);
                if (string.IsNullOrWhiteSpace(deliver) || !File.Exists(deliver)) throw new InvalidDataException("No finalized media asset was returned.");
                await ProjectIngestService.ProcessAsync(OutDirFolder, progress);
                _lastStatistics = rec.Statistics; _lastSelection = rec.Selection;
                CurrentRecordingState = RecordingState.Completed;
                OnPropertyChanged(nameof(CurrentRecordingState));
                RecordingPerfProbe.Mark("recording-save-pipeline-completed");
                saved = true;
                string? notice = rec.Result?.FinalizationNotice;
                if (_storageAutoStopMessage is not null || notice is not null)
                    MessageBox.Show(savingWindow,
                        string.Join("\n\n", new[] { _storageAutoStopMessage, notice, "Saved recording:\n" + OutDirFolder }
                            .Where(text => !string.IsNullOrWhiteSpace(text))),
                        "Low disk space", MessageBoxButton.OK, MessageBoxImage.Warning);
            }
            catch (Exception error)
            {
                RecordingError = UserError.Message(error); CurrentRecordingState = RecordingState.Failed;
                OnPropertyChanged(nameof(RecordingError)); OnPropertyChanged(nameof(CurrentRecordingState));
                DebugLog.WriteError("[VM] Finalization/project persistence failed: " + UserError.Details(error));
                MessageBox.Show(savingWindow, $"Recording could not be completed:\n{UserError.Message(error)}\n\nPartial files were preserved in:\n{OutDirFolder}",
                    "Recording", MessageBoxButton.OK, MessageBoxImage.Error);
            }
            finally
            {
                rec.StateChanged -= RecordingService_StateChanged;
                try { await rec.DisposeAsync(); }
                catch (Exception error)
                {
                    saved = false; RecordingError = UserError.Message(error); CurrentRecordingState = RecordingState.Failed;
                    DebugLog.WriteError("[VM] Awaited recording cleanup failed: " + UserError.Details(error));
                }
                _rec = null; _isRecording = false; _isPaused = false;
                _isSavingRecording = false; _stopStarted = false; _pauseBusy = false;
                savingWindow.AllowClose = true; savingWindow.Close();
                IsEditEnabled = true; ResetUiPreviewSuppression(); RefreshRecordingCommands();
            }
            try
            {
                if (_exitAfterSave)
                {
                    await CloseAfterCleanupAsync();
                    return;
                }
                if (saved)
                {
                    var home = new HomeScreenWindow { WindowStartupLocation = WindowStartupLocation.CenterScreen };
                    home.Show(); System.Windows.Application.Current.MainWindow = home;
                    RecordingPerfProbe.Mark("recording-navigation-home-opened");
                    await CloseAfterCleanupAsync();
                }
                else
                {
                    IsRecordingPanelVisible = false; IsSettingsPanelVisible = true; IsRecButtonVisible = true;
                    TextRecTimer = "00:00:00"; PauseResumeText = "Pause";
                    RecordingScreenWindow.IsEnabled = true; RecordingScreenWindow.Show(); RecordingScreenWindow.Activate();
                    UpdateScreenPreviewRunning(); UpdateWebcamPreviewRunning();
                }
            }
            catch (Exception error) { DebugLog.Write("[VM] Post-finalization window cleanup: " + UserError.Details(error)); }
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
                    ApplyDrawArea(win.Result.Value);
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
        private void ApplyDrawArea(System.Windows.Rect region)
        {
            int w = ((int)Math.Round(region.Width)) & ~1;
            int h = ((int)Math.Round(region.Height)) & ~1;
            region = new System.Windows.Rect(region.X, region.Y, w, h);
            var monitor = FindSingleScreenContainingCrop(region);
            if (monitor is null || w < 2 || h < 2)
            {
                MessageBox.Show("Select a region entirely within one physical monitor.",
                    "Draw Area", System.Windows.MessageBoxButton.OK, System.Windows.MessageBoxImage.Information);
                return;
            }
            _lastDrawArea = region;
            DrawAreaInfoText = $"Draw Area: {w} x {h}";
            IsDrawAreaInfoVisible = true;
            var drawItem = ScreenOptions.FirstOrDefault(o => o.Value == null &&
                         string.Equals(o.Display, DrawAreaLabel, StringComparison.OrdinalIgnoreCase));
            if (drawItem != null)
            {
                _suppressSelectionHandler = true;
                SelectedScreenOption = drawItem;
                _suppressSelectionHandler = false;
            }
            SelectedScreen = monitor;
            UpdateScreenPreviewRunning();
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

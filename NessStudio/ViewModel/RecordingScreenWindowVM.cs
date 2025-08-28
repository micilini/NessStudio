using DirectShowLib;
using NAudio.CoreAudioApi;
using NessStudio.Components.Header;
using NessStudio.Components.Menu;
using NessStudio.Components.RecentProjects;
using NessStudio.Models;
using NessStudio.View.DrawAreaScreen;
using NessStudio.View.HomeScreen;
using NessStudio.View.RecordingScreen;
using NessStudio.ViewModel.Commands;
using NessStudio.ViewModel.Helpers;
using OpenCvSharp;
using OpenCvSharp.WpfExtensions;
using System;
using System.Collections.Generic;
using System.Collections.ObjectModel;
using System.ComponentModel;
using System.Drawing;
using System.Drawing;
using System.Drawing.Imaging;
using System.IO;
using System.Linq;
using System.Runtime.InteropServices;
using System.Text;
using System.Threading.Tasks;
using System.Windows;
using System.Windows.Forms;
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

        private bool _pauseBusy;

        private bool _stopStarted = false;


        private bool _suppressSelectionHandler = false;

        // ==== FLAGS (switches) ====
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

        private VideoCapture _webcamCapture;
        private CancellationTokenSource _webcamCts;
        private bool _isLoaded = false;

        private bool _isRecording = false;
        private bool _isPaused = false;
        private DispatcherTimer _recTimer;
        private TimeSpan _elapsed;

        private bool _isClosing = false;

        private ViewModel.Helpers.RecordAssist _rec;

        public ObservableCollection<System.Windows.Forms.Screen> Screens { get; } = new();
        public ObservableCollection<DirectShowLib.DsDevice> Webcams { get; } = new();
        public ObservableCollection<NAudio.CoreAudioApi.MMDevice> Microphones { get; } = new();

        public PlayRecodingCommand PlayRecodingCommand { get; set; }
        public PauseRecordingCommand PauseRecordingCommand { get; set; }
        public StopRecordingCommand StopRecordingCommand { get; set; }

        private const string DrawAreaLabel = "Draw Area";

        public RecordingScreenWindowVM(RecordingScreenWindow recordingScreen)
        {
            RecordingScreenWindow = recordingScreen;

            PlayRecodingCommand = new PlayRecodingCommand(this);
            PauseRecordingCommand = new PauseRecordingCommand(this);
            StopRecordingCommand = new StopRecordingCommand(this);

            RecordingScreenWindow.Loaded += RecordingScreenWindow_Loaded;
            RecordingScreenWindow.Unloaded += RecordingScreenWindow_Unloaded;
            RecordingScreenWindow.Closing += RecordingScreenWindow_Closing;

            IsScreenEnabled = false;
            IsRecordingPanelVisible = false;
            IsSettingsPanelVisible = true;
        }

        private void RecordingScreenWindow_Loaded(object sender, RoutedEventArgs e)
        {
            try
            {
                PopulateScreens();
                PopulateWebcams();
                PopulateMicrophonesAndSystem();

                _isLoaded = true;
                UpdateScreenPreviewRunning();
                UpdateWebcamPreviewRunning();

                _recTimer = new DispatcherTimer(DispatcherPriority.Background);
                _recTimer.Interval = TimeSpan.FromSeconds(1);
                _recTimer.Tick += (s, e) =>
                {
                    _elapsed = _elapsed.Add(TimeSpan.FromSeconds(1));
                    TextRecTimer = _elapsed.ToString(@"hh\:mm\:ss");
                };

            }
            catch (Exception ex)
            {
                System.Windows.MessageBox.Show($"Initialization error: {ex.Message}", "Error", MessageBoxButton.OK, MessageBoxImage.Error);
            }
        }

        private void RecordingScreenWindow_Unloaded(object sender, RoutedEventArgs e)
        {
            StopScreenPreview();
            StopWebcamPreview();
        }

        private void RecordingScreenWindow_Closing(object? sender, CancelEventArgs e)
        {
            if (_isClosing)
            {
                return;
            }

            StopScreenPreview();
            StopWebcamPreview();

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

                    if (_isRecording && _rec != null)
                    {
                        await _rec.StopAsync();
                    }
                }
                catch
                {
                    
                }
                finally
                {
                    try { _rec?.Dispose(); } catch { }
                    _rec = null;
                    _isRecording = false;
                    _isPaused = false;

                    try
                    {
                        NessStudio.ViewModel.Helpers.FfmpegOrphansKiller.KillIfMatchesAppModules();
                    }
                    catch {  }
                }
                try
                {
                    RecordingScreenWindow.Close();
                }
                catch { }
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
            if (IsScreenEnabled && SelectedScreen != null)
                StartScreenPreview();
            else
                StopScreenPreview();
        }

        private void StartScreenPreview()
        {
            if (_screenCts != null) return;

            _screenCts = new CancellationTokenSource();
            var token = _screenCts.Token;

            int rev = System.Threading.Interlocked.Increment(ref _screenRev);

            var scr = SelectedScreen ?? System.Windows.Forms.Screen.PrimaryScreen;
            var srcBounds = scr.Bounds;
            int W = srcBounds.Width;
            int H = srcBounds.Height;

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
            try
            {
                System.Threading.Interlocked.Increment(ref _screenRev);

                _screenCts?.Cancel();
                _screenCts?.Dispose();
                _screenCts = null;

                System.Threading.Interlocked.Exchange(ref _screenPending, 0);
            }
            catch { }
            ScreenPreviewImage = null;
        }

        private void RestartScreenPreview()
        {
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
            if (IsWebcamEnabled && SelectedWebcam != null)
                StartWebcamPreview();
            else
                StopWebcamPreview();
        }

        private void StartWebcamPreview()
        {
            if (SelectedWebcam == null) return;

            StopWebcamPreview();

            string camName = SelectedWebcam.Name;
            int camIndex = Webcams?.IndexOf(SelectedWebcam) ?? -1;

            _webcamCts = new CancellationTokenSource();
            var token = _webcamCts.Token;

            Task.Run(() =>
            {
                VideoCapture cap = null;
                Mat frame = null;
                try
                {
                    cap = new VideoCapture($"video={camName}", VideoCaptureAPIs.DSHOW);
                    if (!cap.IsOpened())
                    {
                        cap.Dispose();
                        cap = new VideoCapture($"video={camName}", VideoCaptureAPIs.MSMF);
                    }
                    if (!cap.IsOpened() && camIndex >= 0)
                    {
                        cap.Dispose();
                        cap = new VideoCapture(camIndex, VideoCaptureAPIs.DSHOW);
                    }
                    if (!cap.IsOpened() && camIndex >= 0)
                    {
                        cap.Dispose();
                        cap = new VideoCapture(camIndex, VideoCaptureAPIs.MSMF);
                    }
                    if (!cap.IsOpened())
                    {
                        RecordingScreenWindow?.Dispatcher?.BeginInvoke(new Action(() =>
                            System.Windows.MessageBox.Show("Could not open the selected webcam.", "Webcam",
                                MessageBoxButton.OK, MessageBoxImage.Warning)));
                        return;
                    }

                    _webcamCapture = cap;

                    cap.Set(VideoCaptureProperties.FrameWidth, 640);
                    cap.Set(VideoCaptureProperties.FrameHeight, 360);
                    cap.Set(VideoCaptureProperties.Fps, 30);

                    frame = new Mat();

                    while (!token.IsCancellationRequested)
                    {
                        if (cap == null || !cap.IsOpened())
                        {
                            Thread.Sleep(10);
                            continue;
                        }

                        if (!cap.Read(frame) || frame.Empty())
                        {
                            Thread.Sleep(10);
                            continue;
                        }

                        var src = OpenCvSharp.WpfExtensions.BitmapSourceConverter.ToBitmapSource(frame);
                        src.Freeze();

                        RecordingScreenWindow?.Dispatcher?.BeginInvoke(new Action(() =>
                        {
                            WebcamPreviewImage = src;
                        }), DispatcherPriority.Render);

                        Thread.Sleep(15);
                    }
                }
                catch (Exception ex)
                {
                    RecordingScreenWindow?.Dispatcher?.BeginInvoke(new Action(() =>
                        System.Windows.MessageBox.Show($"Webcam error: {ex.Message}", "Error",
                            MessageBoxButton.OK, MessageBoxImage.Error)));
                }
                finally
                {
                    try
                    {
                        frame?.Dispose();

                        if (cap != null)
                        {
                            try { cap.Release(); } catch { }
                            try { cap.Dispose(); } catch { }
                        }
                        _webcamCapture = null;

                        RecordingScreenWindow?.Dispatcher?.BeginInvoke(new Action(() =>
                        {
                            WebcamPreviewImage = null;
                        }));
                    }
                    catch { }
                }
            }, token);
        }

        private void StopWebcamPreview()
        {
            try
            {
                _webcamCts?.Cancel();
                _webcamCts?.Dispose();
                _webcamCts = null;

                RecordingScreenWindow?.Dispatcher?.BeginInvoke(new Action(() =>
                {
                    WebcamPreviewImage = null;
                }));
            }
            catch { }
        }


        private void RestartWebcamPreview()
        {
            StopWebcamPreview();
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

        private async void BtnRec_Click()
        {
            if (_isRecording) return;

            bool isDrawArea = _lastDrawArea.HasValue && IsDrawAreaInfoVisible;

            bool hasAny = IsScreenEnabled || IsWebcamEnabled || IsMicrophoneEnabled || IsSystemAudioEnabled || isDrawArea;
            if (!hasAny)
            {
                System.Windows.MessageBox.Show(
                    "You need to enable at least one recording option to continue.",
                    "Attention",
                    MessageBoxButton.OK,
                    MessageBoxImage.Warning
                );
                return;
            }

            IsEditEnabled = false;
            IsRecButtonVisible = false;

            var paths = NessStudio.ViewModel.Helpers.OutputDirectoryService.BuildPaths();
            OutDirFolder = paths.BaseDir;

            var screen = IsScreenEnabled ? SelectedScreen : null;

            StopScreenPreview();
            StopWebcamPreview();
            await Task.Delay(800);

            IsCountdownVisible = true;
            for (int i = 3; i >= 1; i--)
            {
                CountdownText = i.ToString();
                await Task.Delay(1000);
            }
            IsCountdownVisible = false;

            var selection = new NessStudio.Models.RecordingDeviceSelection(
                webcamName: (IsWebcamEnabled && SelectedWebcam != null) ? SelectedWebcam.Name : null,
                micDeviceId: (IsMicrophoneEnabled && SelectedMicrophone != null) ? SelectedMicrophone.ID : null,
                loopbackDeviceId: (IsSystemAudioEnabled && SelectedRenderLoopback != null) ? SelectedRenderLoopback.ID : null,
                displayFriendlyName: (SelectedScreen != null) ? GetDisplayFriendlyName(SelectedScreen.DeviceName) : null
            );

            if (!selection.AnyAudio && !selection.AnyVideo && !IsScreenEnabled && !isDrawArea)
            {
                System.Windows.MessageBox.Show(
                    "Select at least one video or audio source.",
                    "Recording",
                    MessageBoxButton.OK,
                    MessageBoxImage.Warning
                );
                return;
            }

            var targets = selection.BuildTargets(screen);
            System.Windows.Rect? crop = isDrawArea ? _lastDrawArea.Value : (System.Windows.Rect?)null;

            _rec = new NessStudio.ViewModel.Helpers.RecordAssist(paths, targets, crop);

            _isRecording = true;
            _isPaused = false;
            _elapsed = TimeSpan.Zero;

            IsRecordingPanelVisible = true;
            IsSettingsPanelVisible = false;
            TextRecTimer = "00:00:00";
            PauseResumeText = "Pause";

            await System.Windows.Threading.Dispatcher.Yield(System.Windows.Threading.DispatcherPriority.Render);

            _recTimer.Start();
            IsEditEnabled = true;

            try
            {  
                await Task.Run(async () => await _rec.StartAsync());
            }
            catch (Exception ex)
            {
                System.Windows.MessageBox.Show($"Failed to start recording:\n{ex.Message}", "Recording",
                    MessageBoxButton.OK, MessageBoxImage.Error);

                _isRecording = false;
                _isPaused = false;
                _recTimer.Stop();

                IsRecordingPanelVisible = false;
                IsSettingsPanelVisible = true;
                IsRecButtonVisible = true;
                return;
            }
        }

        private async void BtnPauseResume_Click()
        {
            if (_pauseBusy || !_isRecording || _rec == null) return;
            _pauseBusy = true;

            try
            {
                if (!_isPaused)
                {
                    _recTimer.Stop();
                    PauseResumeText = "Resume";
                    ButtonPlayPauseIcon = "/Assets/Images/play-icon.png";
                    await System.Windows.Threading.Dispatcher.Yield(System.Windows.Threading.DispatcherPriority.Render);
                    await _rec.PauseAsync();
                    _isPaused = true;
                }
                else
                {
                    PauseResumeText = "Pause";
                    ButtonPlayPauseIcon = "/Assets/Images/pause-icon.png";
                    await System.Windows.Threading.Dispatcher.Yield(System.Windows.Threading.DispatcherPriority.Render);
                    await _rec.ResumeAsync();
                    _recTimer.Start();
                    _isPaused = false;
                }
            }
            catch (Exception ex)
            {
                System.Windows.MessageBox.Show($"Pause/Resume error:\n{ex.Message}", "Recording",
                    MessageBoxButton.OK, MessageBoxImage.Error);
            }
            finally
            {
                _pauseBusy = false;
            }
        }

        private async void BtnStop_Click()
        {
            if (!_isRecording || _rec == null) return;

            IsEditEnabled = false;

            _recTimer.Stop();

            try
            {
                string deliver = await _rec.StopAndFinalizeAsync();

                if (!string.IsNullOrWhiteSpace(OutDirFolder))
                {
                    FileCleanup.DeleteTxtAndLogArtifactsFromDeliver(OutDirFolder, recurse: false);
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

                if (!string.IsNullOrWhiteSpace(OutDirFolder))
                {
                    try
                    {
                        var _ = await NessStudio.ViewModel.Helpers.ProjectIngestService.ProcessAsync(OutDirFolder);
                    }
                    catch (Exception ingestEx)
                    {
                        System.Windows.MessageBox.Show(
                            $"Failed to save recording:\n{ingestEx.Message}",
                            "Danger",
                            MessageBoxButton.OK,
                            MessageBoxImage.Warning
                        );
                    }
                }
            }
            catch (Exception ex)
            {
                System.Windows.MessageBox.Show($"Stop error:\n{ex.Message}", "Recording",
                    MessageBoxButton.OK, MessageBoxImage.Error);
            }
            finally
            {
                _isRecording = false;
                _isPaused = false;

                _rec?.Dispose();
                _rec = null;
            }

            try
            {
                RecordingScreenWindow.Hide();

                var home = new HomeScreenWindow
                {
                    Owner = RecordingScreenWindow,
                    WindowStartupLocation = WindowStartupLocation.CenterOwner
                };

                home.Show();
                home.Owner = null;

                System.Windows.Application.Current.ShutdownMode = ShutdownMode.OnLastWindowClose;
                System.Windows.Application.Current.MainWindow = home;

                RecordingScreenWindow.Close();
            }
            catch (Exception navEx)
            {
                System.Windows.MessageBox.Show($"Error when try to return to Home:\n{navEx.Message}", "Navigation",
                    MessageBoxButton.OK, MessageBoxImage.Error);
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

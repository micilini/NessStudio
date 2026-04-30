using NessStudio.ViewModel.Helpers;
using System;
using System.IO;
using System.Linq;
using System.Threading;
using System.Threading.Tasks;
using System.Windows.Media.Imaging;
using global::Windows.Devices.Enumeration;
using global::Windows.Graphics.DirectX.Direct3D11;
using global::Windows.Graphics.Imaging;
using global::Windows.Media.Capture;
using global::Windows.Media.Capture.Frames;
using global::Windows.Media.MediaProperties;
using global::Windows.Storage;

namespace NessStudio.Recording.Windows
{
    public sealed class MediaCaptureWebcamSession
    {
        public static MediaCaptureWebcamSession Shared { get; } = new MediaCaptureWebcamSession();

        private const uint PreferredRecordWidth = 1280;
        private const uint PreferredRecordHeight = 720;
        private const double PreferredRecordFps = 30.0;

        private const uint SafeRecordWidth = 640;
        private const uint SafeRecordHeight = 480;
        private const double SafeRecordFps = 30.0;

        private readonly SemaphoreSlim _gate = new SemaphoreSlim(1, 1);

        private MediaCapture _capture;
        private LowLagMediaRecording _recording;
        private MediaFrameReader _previewReader;
        private Action<BitmapSource> _previewFrameCallback;
        private MediaCaptureVideoProfile _selectedVideoProfile;
        private MediaCaptureVideoProfileMediaDescription _selectedRecordDescription;
        private MediaCaptureVideoProfileMediaDescription _selectedPreviewDescription;
        private string _deviceId;
        private string _friendlyName;
        private string _preparedOutputPath;
        private int _previewFrameGate = 0;

        private readonly SemaphoreSlim _previewProbeGate = new SemaphoreSlim(1, 1);
        private TaskCompletionSource<bool> _previewFirstFrameTcs;
        private string _activePreviewSourceId;
        private string _activePreviewSubtype;

        private DateTime? _sessionInitializedAtUtc;
        private DateTime? _previewStartedAtUtc;
        private DateTime? _firstPreviewFrameAtUtc;
        private DateTime? _previewStableAtUtc;
        private DateTime? _prepareStartedAtUtc;
        private DateTime? _prepareCompletedAtUtc;
        private DateTime? _startRequestedAtUtc;
        private DateTime? _recordingArmedAtUtc;
        private DateTime? _recordingStoppedAtUtc;
        private string _lastPreparedOutputPath;
        private int _previewFramesObserved;

        public bool IsInitialized => _capture != null;
        public bool IsPrepared => _recording != null;
        public bool IsPreviewRunning => _previewReader != null;
        public bool IsRecording { get; private set; }
        public string FriendlyName => _friendlyName;
        public string PreparedOutputPath => _preparedOutputPath;

        public bool IsPreviewStable => _previewStableAtUtc.HasValue;

        private MediaCaptureWebcamSession()
        {
        }

        public async Task EnsureInitializedAsync(string webcamFriendlyName)
        {
            await _gate.WaitAsync().ConfigureAwait(false);
            try
            {
                string resolvedDeviceId = await ResolveVideoDeviceIdAsync(webcamFriendlyName).ConfigureAwait(false);

                if (_capture != null &&
                    string.Equals(_deviceId, resolvedDeviceId, StringComparison.OrdinalIgnoreCase))
                {
                    DebugLog.Write($"[WebcamSession] EnsureInitializedAsync reuse | friendlyName={_friendlyName}");
                    return;
                }

                DebugLog.Write($"[WebcamSession] EnsureInitializedAsync begin | friendlyName={webcamFriendlyName}");

                await ReleaseCoreAsync().ConfigureAwait(false);

                _capture = new MediaCapture();

                var settings = new MediaCaptureInitializationSettings
                {
                    VideoDeviceId = resolvedDeviceId,
                    StreamingCaptureMode = StreamingCaptureMode.Video,
                    MemoryPreference = MediaCaptureMemoryPreference.Cpu,
                    SharingMode = MediaCaptureSharingMode.ExclusiveControl
                };

                bool usedVideoProfile = TryApplyPreferredVideoProfileSettings(resolvedDeviceId, settings);

                await _capture.InitializeAsync(settings).AsTask().ConfigureAwait(false);

                if (!usedVideoProfile)
                {
                    await TryApplyPreferredStreamPropertiesFallbackAsync().ConfigureAwait(false);
                }

                _deviceId = resolvedDeviceId;
                _friendlyName = webcamFriendlyName;
                _sessionInitializedAtUtc = DateTime.UtcNow;

                LogCurrentVideoConfiguration("initialized");

                DebugLog.Write(
                    $"[WebcamSession] initialized | friendlyName={_friendlyName} | " +
                    $"deviceId={_deviceId} | sessionInitializedAt={_sessionInitializedAtUtc:O}");
            }
            finally
            {
                _gate.Release();
            }
        }


        public async Task StartPreviewAsync(string webcamFriendlyName, Action<BitmapSource> frameCallback)
        {
            if (frameCallback == null)
                throw new ArgumentNullException(nameof(frameCallback));

            await EnsureInitializedAsync(webcamFriendlyName).ConfigureAwait(false);

            await _gate.WaitAsync().ConfigureAwait(false);
            try
            {
                if (_capture == null)
                    throw new InvalidOperationException("Webcam session is not initialized.");

                _previewFrameCallback = frameCallback;

                if (_previewReader != null)
                {
                    DebugLog.Write($"[WebcamSession] StartPreviewAsync reuse | friendlyName={_friendlyName}");
                    return;
                }

                _previewFramesObserved = 0;
                _previewStartedAtUtc = DateTime.UtcNow;
                _firstPreviewFrameAtUtc = null;
                _previewStableAtUtc = null;
                _activePreviewSourceId = null;
                _activePreviewSubtype = null;

                var previewStartResult = await CreateAndStartPreviewReaderWithFallbackAsync().ConfigureAwait(false);
                _previewReader = previewStartResult.Reader;

                DebugLog.Write(
                    $"[WebcamSession] preview started | friendlyName={_friendlyName} | " +
                    $"source={previewStartResult.Source.Info.Id} | " +
                    $"streamType={previewStartResult.Source.Info.MediaStreamType} | " +
                    $"startedAt={_previewStartedAtUtc:O}");
            }
            finally
            {
                _gate.Release();
            }
        }

        public async Task StopPreviewAsync(bool clearCallback = true)
        {
            await _gate.WaitAsync().ConfigureAwait(false);
            try
            {
                await StopPreviewCoreAsync(clearCallback).ConfigureAwait(false);
                DebugLog.Write($"[WebcamSession] preview stopped | friendlyName={_friendlyName}");
            }
            finally
            {
                _gate.Release();
            }
        }

        public async Task PrepareRecordingAsync(string outputPath)
        {
            await _gate.WaitAsync().ConfigureAwait(false);
            try
            {
                if (_capture == null)
                    throw new InvalidOperationException("Webcam session is not initialized.");

                if (IsRecording)
                {
                    DebugLog.Write("[WebcamSession] PrepareRecordingAsync skipped because session is already recording.");
                    return;
                }

                if (_recording != null &&
                    string.Equals(_preparedOutputPath, outputPath, StringComparison.OrdinalIgnoreCase))
                {
                    DebugLog.Write($"[WebcamSession] PrepareRecordingAsync reuse | output={outputPath}");
                    return;
                }

                _prepareStartedAtUtc = DateTime.UtcNow;

                await DisposePreparedRecordingCoreAsync().ConfigureAwait(false);

                string folderPath = Path.GetDirectoryName(outputPath)!;
                string fileName = Path.GetFileName(outputPath);

                StorageFolder folder = await StorageFolder.GetFolderFromPathAsync(folderPath).AsTask().ConfigureAwait(false);
                StorageFile file = await folder.CreateFileAsync(fileName, CreationCollisionOption.ReplaceExisting).AsTask().ConfigureAwait(false);

                var profile = MediaEncodingProfile.CreateMp4(VideoEncodingQuality.Auto);
                ApplySelectedEncodingToProfile(profile);

                _recording = await _capture
                    .PrepareLowLagRecordToStorageFileAsync(profile, file)
                    .AsTask()
                    .ConfigureAwait(false);

                _preparedOutputPath = outputPath;
                _lastPreparedOutputPath = outputPath;
                _prepareCompletedAtUtc = DateTime.UtcNow;

                double sinceSessionInitMs = _sessionInitializedAtUtc.HasValue
                    ? (_prepareCompletedAtUtc.Value - _sessionInitializedAtUtc.Value).TotalMilliseconds
                    : -1.0;

                double sincePreviewStartMs = _previewStartedAtUtc.HasValue
                    ? (_prepareCompletedAtUtc.Value - _previewStartedAtUtc.Value).TotalMilliseconds
                    : -1.0;

                double sincePreviewStableMs = _previewStableAtUtc.HasValue
                    ? (_prepareCompletedAtUtc.Value - _previewStableAtUtc.Value).TotalMilliseconds
                    : -1.0;

                double prepareDurationMs = (_prepareCompletedAtUtc.Value - _prepareStartedAtUtc.Value).TotalMilliseconds;

                DebugLog.Write(
                    $"[WebcamSession] recording prepared | output={outputPath} | " +
                    $"prepareDuration={prepareDurationMs:F0}ms | sinceSessionInit={sinceSessionInitMs:F0}ms | " +
                    $"sincePreviewStart={sincePreviewStartMs:F0}ms | sincePreviewStable={sincePreviewStableMs:F0}ms");
            }
            finally
            {
                _gate.Release();
            }
        }

        public async Task StartPreparedRecordingAsync(int warmupMilliseconds = 0)
        {
            await _gate.WaitAsync().ConfigureAwait(false);
            try
            {
                if (_recording == null)
                    throw new InvalidOperationException("Webcam recording is not prepared.");

                if (IsRecording)
                    return;

                _startRequestedAtUtc = DateTime.UtcNow;
                DateTime requestedAtUtc = _startRequestedAtUtc.Value;

                double previewAgeMs = _previewStartedAtUtc.HasValue
                    ? (requestedAtUtc - _previewStartedAtUtc.Value).TotalMilliseconds
                    : -1.0;

                double stableAgeMs = _previewStableAtUtc.HasValue
                    ? (requestedAtUtc - _previewStableAtUtc.Value).TotalMilliseconds
                    : -1.0;

                double prepareLeadMs = _prepareCompletedAtUtc.HasValue
                    ? (requestedAtUtc - _prepareCompletedAtUtc.Value).TotalMilliseconds
                    : -1.0;

                DebugLog.Write(
                    $"[WebcamSession] StartPreparedRecordingAsync begin | " +
                    $"previewRunning={IsPreviewRunning} | previewStable={IsPreviewStable} | " +
                    $"previewFrames={_previewFramesObserved} | previewAge={previewAgeMs:F0}ms | " +
                    $"stableAge={stableAgeMs:F0}ms | prepareLead={prepareLeadMs:F0}ms | output={_preparedOutputPath}");

                bool shouldAwaitPreviewWarmup =
                    warmupMilliseconds > 0 &&
                    IsPreviewRunning &&
                    _previewStartedAtUtc.HasValue;

                if (shouldAwaitPreviewWarmup)
                {
                    await WaitForPreviewStabilityAsync(warmupMilliseconds).ConfigureAwait(false);
                }
                else
                {
                    DebugLog.Write(
                        $"[WebcamSession] WaitForPreviewStabilityAsync skipped | " +
                        $"previewRunning={IsPreviewRunning} | previewStable={IsPreviewStable} | " +
                        $"previewFrames={_previewFramesObserved} | warmup={warmupMilliseconds}ms");
                }

                _recordingArmedAtUtc = DateTime.UtcNow;

                await _recording.StartAsync().AsTask().ConfigureAwait(false);

                IsRecording = true;

                double armedAfterMs = (_recordingArmedAtUtc.Value - requestedAtUtc).TotalMilliseconds;
                double firstFrameLeadMs = _firstPreviewFrameAtUtc.HasValue
                    ? (_recordingArmedAtUtc.Value - _firstPreviewFrameAtUtc.Value).TotalMilliseconds
                    : -1.0;

                double stableLeadMs = _previewStableAtUtc.HasValue
                    ? (_recordingArmedAtUtc.Value - _previewStableAtUtc.Value).TotalMilliseconds
                    : -1.0;

                double preparedLeadMs = _prepareCompletedAtUtc.HasValue
                    ? (_recordingArmedAtUtc.Value - _prepareCompletedAtUtc.Value).TotalMilliseconds
                    : -1.0;

                DebugLog.Write(
                    $"[WebcamSession] StartPreparedRecordingAsync end | " +
                    $"previewRunning={IsPreviewRunning} | previewStable={IsPreviewStable} | " +
                    $"armedAfter={armedAfterMs:F0}ms | firstFrameLead={firstFrameLeadMs:F0}ms | " +
                    $"stableLead={stableLeadMs:F0}ms | preparedLead={preparedLeadMs:F0}ms | " +
                    $"previewFrames={_previewFramesObserved} | output={_preparedOutputPath}");
            }
            finally
            {
                _gate.Release();
            }
        }

        private async Task WaitForPreviewStabilityAsync(int warmupMilliseconds)
        {
            int normalizedWarmupMs = Math.Max(0, warmupMilliseconds);
            if (normalizedWarmupMs <= 0)
                return;

            DateTime waitStartedAtUtc = DateTime.UtcNow;
            DateTime timeoutAtUtc = waitStartedAtUtc.AddMilliseconds(Math.Max(normalizedWarmupMs + 600, 1500));

            DebugLog.Write(
                $"[WebcamSession] WaitForPreviewStabilityAsync begin | " +
                $"warmup={normalizedWarmupMs}ms | previewFrames={_previewFramesObserved}");

            while (DateTime.UtcNow < timeoutAtUtc)
            {
                bool stableEnough =
                    _previewStableAtUtc.HasValue &&
                    (DateTime.UtcNow - _previewStableAtUtc.Value).TotalMilliseconds >= normalizedWarmupMs;

                if (stableEnough)
                {
                    double waitedMs = (DateTime.UtcNow - waitStartedAtUtc).TotalMilliseconds;
                    DebugLog.Write(
                        $"[WebcamSession] WaitForPreviewStabilityAsync satisfied | " +
                        $"waited={waitedMs:F0}ms | previewFrames={_previewFramesObserved}");
                    return;
                }

                await Task.Delay(50).ConfigureAwait(false);
            }

            double timeoutWaitedMs = (DateTime.UtcNow - waitStartedAtUtc).TotalMilliseconds;

            DebugLog.Write(
                $"[WebcamSession] WaitForPreviewStabilityAsync timeout/fallback | " +
                $"waited={timeoutWaitedMs:F0}ms | previewFrames={_previewFramesObserved} | " +
                $"firstPreviewFrame={_firstPreviewFrameAtUtc?.ToString("O") ?? "null"} | " +
                $"previewStable={_previewStableAtUtc?.ToString("O") ?? "null"}");
        }

        public async Task StopRecordingAsync(bool keepSessionAlive = true)
        {
            await _gate.WaitAsync().ConfigureAwait(false);
            try
            {
                bool wasRecording = IsRecording;

                if (_recording != null)
                {
                    try
                    {
                        if (IsRecording)
                            await _recording.StopAsync().AsTask().ConfigureAwait(false);
                    }
                    catch (Exception ex)
                    {
                        DebugLog.Write("[WebcamSession] StopRecordingAsync StopAsync warning:\n" + ex);
                    }

                    try
                    {
                        await _recording.FinishAsync().AsTask().ConfigureAwait(false);
                    }
                    catch (Exception ex)
                    {
                        DebugLog.Write("[WebcamSession] StopRecordingAsync FinishAsync warning:\n" + ex);
                    }
                }

                _recording = null;
                _preparedOutputPath = null;
                IsRecording = false;
                _recordingStoppedAtUtc = DateTime.UtcNow;

                double recordingDurationMs = (_recordingArmedAtUtc.HasValue && _recordingStoppedAtUtc.HasValue)
                    ? (_recordingStoppedAtUtc.Value - _recordingArmedAtUtc.Value).TotalMilliseconds
                    : -1.0;

                double sessionAgeMs = (_sessionInitializedAtUtc.HasValue && _recordingStoppedAtUtc.HasValue)
                    ? (_recordingStoppedAtUtc.Value - _sessionInitializedAtUtc.Value).TotalMilliseconds
                    : -1.0;

                DebugLog.Write(
                    $"[WebcamSession] recording stopped | " +
                    $"keepSessionAlive={keepSessionAlive} | wasRecording={wasRecording} | " +
                    $"previewRunning={IsPreviewRunning} | previewFrames={_previewFramesObserved} | " +
                    $"recordingDuration={recordingDurationMs:F0}ms | sessionAge={sessionAgeMs:F0}ms | " +
                    $"lastPreparedOutput={_lastPreparedOutputPath ?? "null"}");

                if (!keepSessionAlive)
                    await ReleaseCoreAsync().ConfigureAwait(false);
            }
            finally
            {
                _gate.Release();
            }
        }

        public async Task ReleaseAsync()
        {
            await _gate.WaitAsync().ConfigureAwait(false);
            try
            {
                await ReleaseCoreAsync().ConfigureAwait(false);
            }
            finally
            {
                _gate.Release();
            }
        }

        private static double ScoreProfileDescriptionPreferred(MediaCaptureVideoProfileMediaDescription description)
        {
            return ScoreProfileDescriptionInternal(
                description,
                PreferredRecordWidth,
                PreferredRecordHeight,
                PreferredRecordFps,
                preferMjpg: true);
        }

        private static double ScoreProfileDescriptionSafe(MediaCaptureVideoProfileMediaDescription description)
        {
            return ScoreProfileDescriptionInternal(
                description,
                SafeRecordWidth,
                SafeRecordHeight,
                SafeRecordFps,
                preferMjpg: false);
        }

        private static double ScoreEncodingPropertiesPreferred(VideoEncodingProperties properties)
        {
            return ScoreEncodingPropertiesInternal(
                properties,
                PreferredRecordWidth,
                PreferredRecordHeight,
                PreferredRecordFps,
                preferMjpg: true);
        }

        private static double ScoreEncodingPropertiesSafe(VideoEncodingProperties properties)
        {
            return ScoreEncodingPropertiesInternal(
                properties,
                SafeRecordWidth,
                SafeRecordHeight,
                SafeRecordFps,
                preferMjpg: false);
        }

        private static double ScoreProfileDescriptionInternal(
            MediaCaptureVideoProfileMediaDescription description,
            uint targetWidth,
            uint targetHeight,
            double targetFps,
            bool preferMjpg)
        {
            if (description == null)
                return double.MinValue;

            double subtypeScore = GetSubtypeScore(description.Subtype, preferMjpg);
            double resolutionPenalty =
                Math.Abs((double)description.Width - targetWidth) +
                Math.Abs((double)description.Height - targetHeight);
            double fpsPenalty = Math.Abs(description.FrameRate - targetFps) * 100.0;
            double oversizePenalty = (description.Width > 1920 || description.Height > 1080) ? 500.0 : 0.0;

            return subtypeScore - resolutionPenalty - fpsPenalty - oversizePenalty;
        }

        private static double ScoreEncodingPropertiesInternal(
            VideoEncodingProperties properties,
            uint targetWidth,
            uint targetHeight,
            double targetFps,
            bool preferMjpg)
        {
            if (properties == null)
                return double.MinValue;

            double subtypeScore = GetSubtypeScore(properties.Subtype, preferMjpg);
            double resolutionPenalty =
                Math.Abs((double)properties.Width - targetWidth) +
                Math.Abs((double)properties.Height - targetHeight);
            double fpsPenalty = Math.Abs(ReadFrameRate(properties) - targetFps) * 100.0;
            double oversizePenalty = (properties.Width > 1920 || properties.Height > 1080) ? 500.0 : 0.0;

            return subtypeScore - resolutionPenalty - fpsPenalty - oversizePenalty;
        }

        private static double GetSubtypeScore(string subtype, bool preferMjpg)
        {
            if (string.IsNullOrWhiteSpace(subtype))
                return 1000.0;

            switch (subtype.Trim().ToUpperInvariant())
            {
                case "MJPG":
                case "MJPEG":
                    return preferMjpg ? 5000.0 : 3500.0;

                case "NV12":
                    return preferMjpg ? 4000.0 : 4500.0;

                case "YUY2":
                    return 3000.0;

                case "RGB24":
                case "ARGB32":
                case "BGRA8":
                    return 2500.0;

                default:
                    return 1000.0;
            }
        }

        private bool TryApplyPreferredVideoProfileSettings(string deviceId, MediaCaptureInitializationSettings settings)
        {
            try
            {
                _selectedVideoProfile = null;
                _selectedRecordDescription = null;
                _selectedPreviewDescription = null;

                if (!MediaCapture.IsVideoProfileSupported(deviceId))
                {
                    DebugLog.Write("[WebcamSession] video profiles not supported by this webcam/device.");
                    return false;
                }

                var profiles = MediaCapture.FindAllVideoProfiles(deviceId);
                if (profiles == null || profiles.Count == 0)
                {
                    DebugLog.Write("[WebcamSession] FindAllVideoProfiles returned no profiles.");
                    return false;
                }

                var preferredMatch = profiles
                    .SelectMany(profile => profile.SupportedRecordMediaDescription.Select(desc => new
                    {
                        Profile = profile,
                        Description = desc,
                        Score = ScoreProfileDescriptionPreferred(desc),
                        Mode = "preferred"
                    }))
                    .OrderByDescending(x => x.Score)
                    .FirstOrDefault();

                var safeMatch = profiles
                    .SelectMany(profile => profile.SupportedRecordMediaDescription.Select(desc => new
                    {
                        Profile = profile,
                        Description = desc,
                        Score = ScoreProfileDescriptionSafe(desc),
                        Mode = "safe"
                    }))
                    .OrderByDescending(x => x.Score)
                    .FirstOrDefault();

                var chosen = preferredMatch;

                if (chosen == null && safeMatch == null)
                {
                    DebugLog.Write("[WebcamSession] no compatible record media description found in video profiles.");
                    return false;
                }

                if (chosen == null)
                    chosen = safeMatch;
                else if (safeMatch != null)
                {
                    double preferredResolutionDelta =
                        Math.Abs((double)chosen.Description.Width - PreferredRecordWidth) +
                        Math.Abs((double)chosen.Description.Height - PreferredRecordHeight);

                    double preferredFpsDelta = Math.Abs(chosen.Description.FrameRate - PreferredRecordFps);

                    bool preferredLooksWeak =
                        preferredResolutionDelta > 900.0 ||
                        preferredFpsDelta > 8.0;

                    if (preferredLooksWeak)
                    {
                        DebugLog.Write(
                            $"[WebcamSession] preferred profile looked weak | " +
                            $"candidate={FormatProfileDescription(chosen.Description)} | " +
                            $"safeCandidate={FormatProfileDescription(safeMatch.Description)}");
                    }
                }

                _selectedVideoProfile = chosen.Profile;
                _selectedRecordDescription = chosen.Description;
                _selectedPreviewDescription = SelectBestPreviewDescription(chosen.Profile, chosen.Description);

                settings.VideoProfile = _selectedVideoProfile;
                settings.RecordMediaDescription = _selectedRecordDescription;

                if (_selectedPreviewDescription != null)
                    settings.PreviewMediaDescription = _selectedPreviewDescription;

                DebugLog.Write(
                    $"[WebcamSession] preferred profile selected | " +
                    $"mode={chosen.Mode} | " +
                    $"record={FormatProfileDescription(_selectedRecordDescription)} | " +
                    $"preview={FormatProfileDescription(_selectedPreviewDescription)}");

                return true;
            }
            catch (Exception ex)
            {
                DebugLog.Write("[WebcamSession] TryApplyPreferredVideoProfileSettings fallback:\n" + ex);
                _selectedVideoProfile = null;
                _selectedRecordDescription = null;
                _selectedPreviewDescription = null;
                return false;
            }
        }

        private async Task TryApplyPreferredStreamPropertiesFallbackAsync()
        {
            if (_capture == null)
                return;

            try
            {
                var allRecordProps = _capture.VideoDeviceController
                    .GetAvailableMediaStreamProperties(MediaStreamType.VideoRecord)
                    .OfType<VideoEncodingProperties>()
                    .ToList();

                var allPreviewProps = _capture.VideoDeviceController
                    .GetAvailableMediaStreamProperties(MediaStreamType.VideoPreview)
                    .OfType<VideoEncodingProperties>()
                    .ToList();

                if (allRecordProps.Count == 0)
                {
                    DebugLog.Write("[WebcamSession] fallback record props unavailable.");
                    return;
                }

                var preferredRecordProps = allRecordProps
                    .OrderByDescending(ScoreEncodingPropertiesPreferred)
                    .FirstOrDefault();

                var safeRecordProps = allRecordProps
                    .OrderByDescending(ScoreEncodingPropertiesSafe)
                    .FirstOrDefault();

                var recordProps = preferredRecordProps;
                string recordMode = "preferred";

                if (recordProps == null && safeRecordProps != null)
                {
                    recordProps = safeRecordProps;
                    recordMode = "safe";
                }
                else if (recordProps != null)
                {
                    double preferredResolutionDelta =
                        Math.Abs((double)recordProps.Width - PreferredRecordWidth) +
                        Math.Abs((double)recordProps.Height - PreferredRecordHeight);

                    double preferredFpsDelta = Math.Abs(ReadFrameRate(recordProps) - PreferredRecordFps);

                    bool preferredLooksWeak =
                        preferredResolutionDelta > 900.0 ||
                        preferredFpsDelta > 8.0;

                    if (preferredLooksWeak && safeRecordProps != null)
                    {
                        DebugLog.Write(
                            $"[WebcamSession] fallback preferred record looked weak | " +
                            $"preferred={FormatEncodingProperties(recordProps)} | " +
                            $"safe={FormatEncodingProperties(safeRecordProps)}");
                    }
                }

                if (recordProps != null)
                {
                    await _capture.VideoDeviceController
                        .SetMediaStreamPropertiesAsync(MediaStreamType.VideoRecord, recordProps)
                        .AsTask()
                        .ConfigureAwait(false);

                    DebugLog.Write(
                        $"[WebcamSession] fallback record props applied | " +
                        $"mode={recordMode} | {FormatEncodingProperties(recordProps)}");
                }

                double targetAspectRatio = recordProps != null
                    ? GetAspectRatio(recordProps.Width, recordProps.Height)
                    : (16.0 / 9.0);

                double targetFps = recordProps != null
                    ? ReadFrameRate(recordProps)
                    : PreferredRecordFps;

                VideoEncodingProperties previewProps = null;
                string previewMode = "preferred";

                if (allPreviewProps.Count > 0)
                {
                    previewProps = allPreviewProps
                        .OrderBy(p => Math.Abs(GetAspectRatio(p.Width, p.Height) - targetAspectRatio))
                        .ThenBy(p => Math.Abs(ReadFrameRate(p) - targetFps))
                        .ThenByDescending(ScoreEncodingPropertiesPreferred)
                        .FirstOrDefault();

                    if (previewProps == null)
                    {
                        previewProps = allPreviewProps
                            .OrderByDescending(ScoreEncodingPropertiesSafe)
                            .FirstOrDefault();

                        previewMode = "safe";
                    }
                }

                if (previewProps != null)
                {
                    await _capture.VideoDeviceController
                        .SetMediaStreamPropertiesAsync(MediaStreamType.VideoPreview, previewProps)
                        .AsTask()
                        .ConfigureAwait(false);

                    DebugLog.Write(
                        $"[WebcamSession] fallback preview props applied | " +
                        $"mode={previewMode} | {FormatEncodingProperties(previewProps)}");
                }
                else
                {
                    DebugLog.Write("[WebcamSession] fallback preview props unavailable.");
                }
            }
            catch (Exception ex)
            {
                DebugLog.Write("[WebcamSession] TryApplyPreferredStreamPropertiesFallbackAsync warning:\n" + ex);
            }
        }

        private void ApplySelectedEncodingToProfile(MediaEncodingProfile profile)
        {
            if (_capture == null || profile?.Video == null)
                return;

            try
            {
                var currentRecordProps = _capture.VideoDeviceController
                    .GetMediaStreamProperties(MediaStreamType.VideoRecord) as VideoEncodingProperties;

                if (currentRecordProps == null)
                    return;

                profile.Video.Width = currentRecordProps.Width;
                profile.Video.Height = currentRecordProps.Height;

                if (currentRecordProps.FrameRate != null)
                {
                    profile.Video.FrameRate.Numerator = Math.Max(1u, currentRecordProps.FrameRate.Numerator);
                    profile.Video.FrameRate.Denominator = Math.Max(1u, currentRecordProps.FrameRate.Denominator);
                }

                DebugLog.Write($"[WebcamSession] encoding profile aligned | {FormatEncodingProperties(currentRecordProps)}");
            }
            catch (Exception ex)
            {
                DebugLog.Write("[WebcamSession] ApplySelectedEncodingToProfile warning:\n" + ex);
            }
        }

        private static MediaCaptureVideoProfileMediaDescription SelectBestPreviewDescription(
    MediaCaptureVideoProfile profile,
    MediaCaptureVideoProfileMediaDescription recordDescription)
        {
            if (profile?.SupportedPreviewMediaDescription == null || profile.SupportedPreviewMediaDescription.Count == 0)
                return null;

            double targetAspect = recordDescription == null
                ? (16.0 / 9.0)
                : GetAspectRatio(recordDescription.Width, recordDescription.Height);

            double targetFps = recordDescription?.FrameRate ?? PreferredRecordFps;

            return profile.SupportedPreviewMediaDescription
                .OrderBy(d => Math.Abs(GetAspectRatio(d.Width, d.Height) - targetAspect))
                .ThenBy(d => Math.Abs(d.FrameRate - targetFps))
                .ThenByDescending(d => ScoreProfileDescriptionPreferred(d))
                .ThenBy(d => Math.Abs((double)d.Width - PreferredRecordWidth) + Math.Abs((double)d.Height - PreferredRecordHeight))
                .FirstOrDefault();
        }

        private static double ReadFrameRate(VideoEncodingProperties properties)
        {
            if (properties?.FrameRate == null || properties.FrameRate.Denominator == 0)
                return 0.0;

            return (double)properties.FrameRate.Numerator / properties.FrameRate.Denominator;
        }

        private static double GetAspectRatio(uint width, uint height)
        {
            if (height == 0)
                return 0.0;

            return (double)width / height;
        }

        private void LogCurrentVideoConfiguration(string stage)
        {
            if (_capture == null)
                return;

            try
            {
                var recordProps = _capture.VideoDeviceController
                    .GetMediaStreamProperties(MediaStreamType.VideoRecord) as VideoEncodingProperties;

                var previewProps = _capture.VideoDeviceController
                    .GetMediaStreamProperties(MediaStreamType.VideoPreview) as VideoEncodingProperties;

                string characteristic = _capture.MediaCaptureSettings?.VideoDeviceCharacteristic.ToString() ?? "Unknown";

                DebugLog.Write(
                    $"[WebcamSession] {stage} | characteristic={characteristic} | " +
                    $"record={FormatEncodingProperties(recordProps)} | " +
                    $"preview={FormatEncodingProperties(previewProps)}");
            }
            catch (Exception ex)
            {
                DebugLog.Write("[WebcamSession] LogCurrentVideoConfiguration warning:\n" + ex);
            }
        }

        private static string FormatProfileDescription(MediaCaptureVideoProfileMediaDescription description)
        {
            if (description == null)
                return "null";

            return $"{description.Width}x{description.Height}@{description.FrameRate:F2} [{description.Subtype}]";
        }

        private static string FormatEncodingProperties(VideoEncodingProperties properties)
        {
            if (properties == null)
                return "null";

            return $"{properties.Width}x{properties.Height}@{ReadFrameRate(properties):F2} [{properties.Subtype}]";
        }

        private async Task DisposePreparedRecordingCoreAsync()
        {
            if (_recording == null)
                return;

            try
            {
                await _recording.FinishAsync().AsTask().ConfigureAwait(false);
            }
            catch (Exception ex)
            {
                DebugLog.Write("[WebcamSession] DisposePreparedRecordingCoreAsync warning:\n" + ex);
            }
            finally
            {
                _recording = null;
                _preparedOutputPath = null;
                IsRecording = false;
            }
        }

        private async Task ReleaseCoreAsync()
        {
            await StopPreviewCoreAsync(clearCallback: true).ConfigureAwait(false);
            await DisposePreparedRecordingCoreAsync().ConfigureAwait(false);

            try
            {
                _capture?.Dispose();
            }
            catch (Exception ex)
            {
                DebugLog.Write("[WebcamSession] ReleaseCoreAsync dispose warning:\n" + ex);
            }

            _capture = null;
            _selectedVideoProfile = null;
            _selectedRecordDescription = null;
            _selectedPreviewDescription = null;
            _deviceId = null;
            _friendlyName = null;
            _preparedOutputPath = null;
            _lastPreparedOutputPath = null;
            _sessionInitializedAtUtc = null;
            _prepareStartedAtUtc = null;
            _prepareCompletedAtUtc = null;
            _startRequestedAtUtc = null;
            _recordingArmedAtUtc = null;
            _recordingStoppedAtUtc = null;
            IsRecording = false;

            DebugLog.Write("[WebcamSession] released");
        }

        private List<MediaFrameSource> GetCandidatePreviewSources()
        {
            if (_capture == null)
                return new List<MediaFrameSource>();

            var sources = _capture.FrameSources?.Values?.ToList();
            if (sources == null || sources.Count == 0)
                return new List<MediaFrameSource>();

            return sources
                .Where(s => s.Info.SourceKind == MediaFrameSourceKind.Color)
                .OrderBy(s => s.Info.MediaStreamType == MediaStreamType.VideoPreview ? 0 : 1)
                .ThenBy(s => s.Info.MediaStreamType == MediaStreamType.VideoRecord ? 0 : 1)
                .ThenBy(s => s.Info.Id, StringComparer.OrdinalIgnoreCase)
                .ToList();
        }

        private async Task<MediaFrameReader> CreatePreviewReaderAsync(MediaFrameSource frameSource, string subtype = null)
        {
            string subtypeLabel = string.IsNullOrWhiteSpace(subtype) ? "native" : subtype;

            DebugLog.Write(
                $"[WebcamSession] CreatePreviewReaderAsync try | " +
                $"source={frameSource?.Info?.Id} | streamType={frameSource?.Info?.MediaStreamType} | subtype={subtypeLabel}");

            if (string.IsNullOrWhiteSpace(subtype))
            {
                return await _capture
                    .CreateFrameReaderAsync(frameSource)
                    .AsTask()
                    .ConfigureAwait(false);
            }

            return await _capture
                .CreateFrameReaderAsync(frameSource, subtype)
                .AsTask()
                .ConfigureAwait(false);
        }

        private static List<string> GetCandidatePreviewSubtypes(MediaFrameSource frameSource)
        {
            var result = new List<string>();

            void AddIfMissing(string value)
            {
                string normalized = string.IsNullOrWhiteSpace(value) ? null : value.Trim();

                bool exists = result.Any(x =>
                    string.Equals(
                        string.IsNullOrWhiteSpace(x) ? null : x.Trim(),
                        normalized,
                        StringComparison.OrdinalIgnoreCase));

                if (!exists)
                    result.Add(normalized);
            }

            
            AddIfMissing(null); 
            AddIfMissing(MediaEncodingSubtypes.Bgra8);
            AddIfMissing(MediaEncodingSubtypes.Nv12);
            AddIfMissing(MediaEncodingSubtypes.Yuy2);

            try
            {
                var currentSubtype = frameSource?.CurrentFormat?.Subtype;
                AddIfMissing(currentSubtype);
            }
            catch
            {
            }

            try
            {
                foreach (var format in frameSource?.SupportedFormats ?? Enumerable.Empty<MediaFrameFormat>())
                {
                    AddIfMissing(format?.Subtype);
                }
            }
            catch
            {
            }

            
            AddIfMissing(MediaEncodingSubtypes.Mjpg);

            return result;
        }

        private static string NormalizePreviewSubtypeLabel(string subtype)
        {
            return string.IsNullOrWhiteSpace(subtype) ? "native" : subtype.Trim();
        }

        private void ResetPreviewProbe(string sourceId, string subtype)
        {
            _activePreviewSourceId = sourceId;
            _activePreviewSubtype = NormalizePreviewSubtypeLabel(subtype);
            _previewFirstFrameTcs = new TaskCompletionSource<bool>(TaskCreationOptions.RunContinuationsAsynchronously);
        }

        private async Task<bool> WaitForFirstPreviewFrameAsync(int timeoutMs)
        {
            var tcs = _previewFirstFrameTcs;
            if (tcs == null)
                return false;

            var completed = await Task.WhenAny(tcs.Task, Task.Delay(timeoutMs)).ConfigureAwait(false);
            return completed == tcs.Task && tcs.Task.Result;
        }

        private async Task<(MediaFrameReader Reader, MediaFrameSource Source)> CreateAndStartPreviewReaderWithFallbackAsync()
        {
            var candidates = GetCandidatePreviewSources();

            if (candidates == null || candidates.Count == 0)
                throw new InvalidOperationException("No compatible preview frame source was found for the webcam.");

            Exception lastError = null;

            foreach (var frameSource in candidates)
            {
                var subtypes = GetCandidatePreviewSubtypes(frameSource);

                DebugLog.Write(
                    $"[WebcamSession] preview candidate try | " +
                    $"source={frameSource.Info.Id} | streamType={frameSource.Info.MediaStreamType} | " +
                    $"subtypes=[{string.Join(", ", subtypes.Select(NormalizePreviewSubtypeLabel))}]");

                foreach (var subtype in subtypes)
                {
                    MediaFrameReader previewReader = null;
                    string subtypeLabel = NormalizePreviewSubtypeLabel(subtype);

                    try
                    {
                        previewReader = await CreatePreviewReaderAsync(frameSource, subtype).ConfigureAwait(false);
                        previewReader.FrameArrived += PreviewReader_FrameArrived;

                        ResetPreviewProbe(frameSource.Info.Id, subtype);

                        MediaFrameReaderStartStatus status =
                            await previewReader.StartAsync().AsTask().ConfigureAwait(false);

                        if (status != MediaFrameReaderStartStatus.Success)
                        {
                            DebugLog.Write(
                                $"[WebcamSession] preview candidate rejected | " +
                                $"source={frameSource.Info.Id} | streamType={frameSource.Info.MediaStreamType} | " +
                                $"subtype={subtypeLabel} | status={status}");

                            try { previewReader.FrameArrived -= PreviewReader_FrameArrived; } catch { }
                            try { previewReader.Dispose(); } catch { }

                            previewReader = null;
                            lastError = new InvalidOperationException(
                                $"Failed to start webcam preview reader. Source={frameSource.Info.Id}, Subtype={subtypeLabel}, Status={status}");
                            continue;
                        }

                        bool gotFirstFrame = await WaitForFirstPreviewFrameAsync(1200).ConfigureAwait(false);
                        if (!gotFirstFrame)
                        {
                            DebugLog.Write(
                                $"[WebcamSession] preview candidate rejected after start | " +
                                $"source={frameSource.Info.Id} | streamType={frameSource.Info.MediaStreamType} | " +
                                $"subtype={subtypeLabel} | reason=no-first-frame");

                            try { await previewReader.StopAsync().AsTask().ConfigureAwait(false); } catch { }
                            try { previewReader.FrameArrived -= PreviewReader_FrameArrived; } catch { }
                            try { previewReader.Dispose(); } catch { }

                            previewReader = null;
                            lastError = new InvalidOperationException(
                                $"Preview reader started but produced no visible frame. Source={frameSource.Info.Id}, Subtype={subtypeLabel}");
                            continue;
                        }

                        DebugLog.Write(
                            $"[WebcamSession] preview candidate accepted | " +
                            $"source={frameSource.Info.Id} | streamType={frameSource.Info.MediaStreamType} | subtype={subtypeLabel}");

                        return (previewReader, frameSource);
                    }
                    catch (Exception ex)
                    {
                        lastError = ex;

                        DebugLog.Write(
                            $"[WebcamSession] preview candidate exception | " +
                            $"source={frameSource.Info.Id} | streamType={frameSource.Info.MediaStreamType} | subtype={subtypeLabel}\n{ex}");

                        if (previewReader != null)
                        {
                            try { await previewReader.StopAsync().AsTask().ConfigureAwait(false); } catch { }
                            try { previewReader.FrameArrived -= PreviewReader_FrameArrived; } catch { }
                            try { previewReader.Dispose(); } catch { }
                        }
                    }
                }
            }

            throw lastError ?? new InvalidOperationException("Failed to start webcam preview reader on all candidate sources/subtypes.");
        }

        private async Task StopPreviewCoreAsync(bool clearCallback)
        {
            var previewReader = _previewReader;
            _previewReader = null;

            if (previewReader != null)
            {
                try { previewReader.FrameArrived -= PreviewReader_FrameArrived; } catch { }

                try
                {
                    await previewReader.StopAsync().AsTask().ConfigureAwait(false);
                }
                catch (Exception ex)
                {
                    DebugLog.Write("[WebcamSession] StopPreviewCoreAsync StopAsync warning:\n" + ex);
                }

                try { previewReader.Dispose(); } catch { }
            }

            if (clearCallback)
                _previewFrameCallback = null;

            _previewStartedAtUtc = null;
            _firstPreviewFrameAtUtc = null;
            _previewStableAtUtc = null;
            _recordingArmedAtUtc = null;
            _previewFramesObserved = 0;
            _activePreviewSourceId = null;
            _activePreviewSubtype = null;
            _previewFirstFrameTcs = null;

            Interlocked.Exchange(ref _previewFrameGate, 0);
        }

        private static SoftwareBitmap TryGetSoftwareBitmapFromPreviewFrame(MediaFrameReference frame)
        {
            if (frame == null)
                return null;

            try
            {
                var softwareBitmap = frame.VideoMediaFrame?.SoftwareBitmap;
                if (softwareBitmap != null)
                    return softwareBitmap;
            }
            catch
            {
            }

            try
            {
                IDirect3DSurface surface = frame.VideoMediaFrame?.Direct3DSurface;
                if (surface != null)
                {
                    return SoftwareBitmap.CreateCopyFromSurfaceAsync(surface)
                        .AsTask()
                        .GetAwaiter()
                        .GetResult();
                }
            }
            catch
            {
            }

            return null;
        }

        private void PreviewReader_FrameArrived(MediaFrameReader sender, MediaFrameArrivedEventArgs args)
        {
            if (Interlocked.Exchange(ref _previewFrameGate, 1) == 1)
                return;

            try
            {
                using var frame = sender.TryAcquireLatestFrame();
                using var softwareBitmap = TryGetSoftwareBitmapFromPreviewFrame(frame);

                if (softwareBitmap == null)
                {
                    if (_previewFramesObserved == 0 || _previewFramesObserved % 60 == 0)
                    {
                        DebugLog.Write("[WebcamSession] preview frame without SoftwareBitmap");
                    }

                    return;
                }

                _previewFramesObserved++;

                if (!_firstPreviewFrameAtUtc.HasValue)
                {
                    _firstPreviewFrameAtUtc = DateTime.UtcNow;

                    try
                    {
                        _previewFirstFrameTcs?.TrySetResult(true);
                    }
                    catch
                    {
                    }

                    DebugLog.Write(
                        $"[WebcamSession] first preview frame | " +
                        $"friendlyName={_friendlyName} | " +
                        $"source={_activePreviewSourceId ?? "unknown"} | " +
                        $"subtype={_activePreviewSubtype ?? "unknown"} | " +
                        $"at={_firstPreviewFrameAtUtc:O}");
                }

                if (!_previewStableAtUtc.HasValue && _previewFramesObserved >= 3)
                {
                    _previewStableAtUtc = DateTime.UtcNow;

                    double firstFrameAfterMs = _previewStartedAtUtc.HasValue
                        ? (_firstPreviewFrameAtUtc.Value - _previewStartedAtUtc.Value).TotalMilliseconds
                        : -1.0;

                    double stableAfterMs = _previewStartedAtUtc.HasValue
                        ? (_previewStableAtUtc.Value - _previewStartedAtUtc.Value).TotalMilliseconds
                        : -1.0;

                    DebugLog.Write(
                        $"[WebcamSession] preview marked stable | " +
                        $"frames={_previewFramesObserved} | firstFrameAfter={firstFrameAfterMs:F0}ms | " +
                        $"stableAfter={stableAfterMs:F0}ms");
                }
                else if (_previewFramesObserved % 30 == 0)
                {
                    DebugLog.Write(
                        $"[WebcamSession] preview progress | " +
                        $"frames={_previewFramesObserved} | stable={_previewStableAtUtc.HasValue}");
                }

                var bitmap = SoftwareBitmapPreviewConverter.TryConvert(softwareBitmap);
                if (bitmap == null)
                {
                    if (_previewFramesObserved == 1 || _previewFramesObserved % 60 == 0)
                    {
                        DebugLog.Write("[WebcamSession] preview bitmap conversion returned null");
                    }

                    return;
                }

                _previewFrameCallback?.Invoke(bitmap);
            }
            catch (Exception ex)
            {
                DebugLog.Write("[WebcamSession] PreviewReader_FrameArrived warning:\n" + ex);
            }
            finally
            {
                Interlocked.Exchange(ref _previewFrameGate, 0);
            }
        }

        private static async Task<string> ResolveVideoDeviceIdAsync(string webcamFriendlyName)
        {
            var devices = await DeviceInformation.FindAllAsync(DeviceClass.VideoCapture).AsTask().ConfigureAwait(false);

            if (devices == null || devices.Count == 0)
                throw new InvalidOperationException("No webcam devices found.");

            if (!string.IsNullOrWhiteSpace(webcamFriendlyName))
            {
                var match = devices.FirstOrDefault(d =>
                    string.Equals(d.Name, webcamFriendlyName, StringComparison.OrdinalIgnoreCase));

                if (match != null)
                    return match.Id;
            }

            return devices[0].Id;
        }
    }
}
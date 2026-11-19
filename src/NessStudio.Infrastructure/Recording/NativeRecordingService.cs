using System.Diagnostics;
using System.Text.Json;
using NessStudio.Diagnostics;
using NessStudio.Interop.Native;
using NessStudio.Models;

namespace NessStudio.Recording.Engines;

/// <summary>Composes native control, bounded observation and final project metadata; never owns media buffers.</summary>
internal sealed class NativeRecordingService(RecordingRequest request, string applicationDirectory, IDiagnosticLog log,
    Func<string, ulong>? availableSpace = null) : IRecordingService
{
    internal static readonly string[] OwnedAssetNames = ["program.mkv", "program.mkv.manifest.json", "program.mkv.manifest.pending", "program.mkv.partial.manifest.json", "program.mkv.partial.manifest.pending", "screen.mkv", "screen.mkv.manifest.json", "screen.mkv.manifest.pending",
        "screen.mkv.partial.manifest.json", "screen.mkv.partial.manifest.pending", "mic.wav", "system.wav", "final.mkv",
        "manifest.json", "manifest.json.pending", "recording-native.json", "recording-native.json.pending", "native-export.json", "native-export.json.pending",
        "webcam.mkv", "webcam.mkv.manifest.json", "webcam.mkv.manifest.pending", "webcam.mkv.partial.manifest.json", "webcam.mp4",
        "webcam.mp4.partial.mp4", "webcam.mp4.export.json", "audio.native.json", "audio.native.json.pending", "native-camera-export.json",
        "recording-startup-error.json", "recording-startup-error.json.pending",
        "native-camera-export.json.pending", "recording-incomplete.json", "recording-incomplete.json.pending", "session.manifest.json", "project-recovery.json", "project-registration.json"];
    internal NativeRecordingWorkspace? Workspace { get; set; }
    private RecordingCameraOptions? _camera;
    private RecordingScene? _scene;
    private static readonly JsonSerializerOptions SceneJson = new() { PropertyNamingPolicy = JsonNamingPolicy.CamelCase };
    private readonly SemaphoreSlim _control = new(1, 1);
    private readonly object _releaseGate = new();
    private readonly CancellationTokenSource _observation = new();
    private readonly CancellationTokenSource _startupCancellation = new();
    private NativeEngine? _engine;
    private NativeSession? _session;
    private Task _observer = Task.CompletedTask;
    private Task? _release;
    private Exception? _failure;
    private int _state, _disposing;
    private bool _started;
    private bool _supportsAudioGain;
    private string? _finalSnapshot;
    private RecordingStatistics _statistics = new(null, null, null);
    private readonly DateTimeOffset _created = DateTimeOffset.UtcNow;
    private DateTimeOffset? _startedUtc, _stoppedUtc;

    public event EventHandler<RecordingStateChangedEventArgs>? StateChanged;
    public RecordingState State => (RecordingState)Volatile.Read(ref _state);
    public RecordingCapabilities Capabilities => NativeRecorderEngineFactory.NativeCapabilities;
    public RecordingSelection? Selection { get; private set; }
    public RecordingStatistics Statistics => Volatile.Read(ref _statistics);
    public RecordingResult? Result { get; private set; }

    private void Publish(RecordingState state, Exception? error = null)
    {
        if (_failure is not null && state is not (RecordingState.Failed or RecordingState.Disposed)) return;
        if (Interlocked.Exchange(ref _state, (int)state) == (int)state && error is null) return;
        foreach (var handler in StateChanged?.GetInvocationList() ?? [])
            try { ((EventHandler<RecordingStateChangedEventArgs>)handler)(this, new(state, error)); }
            catch (Exception observerError) { log.Write("[NativeRecording] Observer: " + UserError.Message(observerError)); }
    }
    private void Fail(Exception error)
    {
        if (Interlocked.CompareExchange(ref _failure, error, null) is null)
        {
            log.Write("[NativeRecording] Failed: " + UserError.Details(error));
            Publish(RecordingState.Failed, error);
        }
    }
    private void EnsureUsable()
    {
        ObjectDisposedException.ThrowIf(Volatile.Read(ref _disposing) != 0, this);
        if (_failure is { } error) throw new InvalidOperationException("The native recording failed: " + UserError.Message(error), error);
    }
    private static void Check(NativeCommandCompletion completion)
    {
        if (completion.ResultCode < 0) throw new NativeEngineException(completion.ResultCode, completion.Stage,
            completion.OriginalHResult, completion.Message);
    }
    private static Exception? SnapshotError(JsonElement root)
    {
        if (!root.TryGetProperty("firstError", out var error) || error.ValueKind != JsonValueKind.Object) return null;
        return new NativeEngineException(error.GetProperty("code").GetInt32(), error.GetProperty("stage").GetUInt32(),
            error.GetProperty("hresult").GetInt32(), error.GetProperty("message").GetString() ?? "Native recording failure");
    }

    public void CancelStartup()
    {
        if (Volatile.Read(ref _disposing) != 0 || _started || State is not
            (RecordingState.Created or RecordingState.Preparing or RecordingState.Ready or RecordingState.Starting or RecordingState.Failed)) return;
        _startupCancellation.Cancel();
        if (Volatile.Read(ref _session) is { } session) _ = InterruptStartupAsync(session);
    }

    private async Task InterruptStartupAsync(NativeSession session)
    {
        try { await session.StopAsync().ConfigureAwait(false); }
        catch (Exception error) { log.Write("[NativeRecording] Startup cancellation: " + UserError.Message(error)); }
    }

    public async Task PrepareAsync()
    {
        await _control.WaitAsync().ConfigureAwait(false);
        try
        {
            EnsureUsable();
            if (State == RecordingState.Ready) return;
            _startupCancellation.Token.ThrowIfCancellationRequested();
            if (State != RecordingState.Created) throw new InvalidOperationException("This session has already been prepared.");
            Publish(RecordingState.Preparing);
            // One background preparation composition contains the synchronous loader,
            // capability/device preflight and JSON plan creation. Commands use their
            // correlated native Tasks directly; no thread per command or media frame.
            await Task.Run(PrepareCoreAsync).ConfigureAwait(false);
            EnsureUsable();
            Publish(RecordingState.Ready);
            _observer = ObserveAsync();
        }
        catch (Exception error) { Fail(error); throw; }
        finally { _control.Release(); }
    }
    private async Task PrepareCoreAsync()
    {
        _startupCancellation.Token.ThrowIfCancellationRequested();
        RecordingStorageSpace.EnsureCanStart(request.OutputDirectory, availableSpace);
        if (!float.IsFinite(request.MicrophoneGain) || request.MicrophoneGain is < 0 or > 1 ||
            !float.IsFinite(request.SystemAudioGain) || request.SystemAudioGain is < 0 or > 1)
            throw new ArgumentException("Recording audio gain must be between 0 and 1.");
        if ((request.Targets.AnyAudio || request.Targets.CaptureWebcam) && !File.Exists(Path.Combine(applicationDirectory, "Native", "FFmpeg", "ffmpeg.exe")))
            throw new FileNotFoundException("Screen/audio recording requires the packaged FFmpeg export dependency.");
        _engine = NativeEngine.Open(applicationDirectory, "NessStudio.MVVM");
        if (_engine.Build.AbiMinor < 8) throw new NotSupportedException("Native recording journals and recovery require libnessstudio ABI 1.8 or newer.");
        using var capabilities = _engine.GetCapabilities();
        var declared = capabilities.RootElement.GetProperty("declaredCapabilities");
        _supportsAudioGain = declared.TryGetProperty("audioGain", out var gainCapability) && gainCapability.GetBoolean();
        if (!_supportsAudioGain && ((request.Targets.MicDeviceId is not null && request.MicrophoneGain != 1) ||
            (request.Targets.LoopbackDeviceId is not null && request.SystemAudioGain != 1)))
            throw new NotSupportedException("The loaded native library does not support recording audio levels. Update the complete NessStudio package.");
        if (!declared.GetProperty("recording").GetBoolean() || !declared.GetProperty("fullSoftware").GetBoolean())
            throw new NotSupportedException("The loaded native library does not implement the required recording capabilities.");
        log.Write("[NativeRecording] capabilities=" + capabilities.RootElement.GetRawText());
        if (Workspace is { } workspace) await workspace.SuspendAsync().ConfigureAwait(false);
        string? id = NativeRecordingWorkspace.ResolveMonitor(_engine, request.Targets.Screen);
        if (request.Targets.CaptureWebcam)
        {
            using var cameras = _engine.GetCameraSources();
            var devices = cameras.RootElement.GetProperty("devices").EnumerateArray().Where(item => request.Camera is { } camera
                ? NativeRecordingSelection.CameraMatches(item, camera.DeviceId) : item.GetProperty("name").GetString() == request.Targets.WebcamName).ToArray();
            if (devices.Length != 1) throw new NotSupportedException("Select an available camera by its stable identity.");
            if (devices[0].TryGetProperty("formatEnumerationHresult", out var enumerationError) && enumerationError.GetInt32() < 0)
            {
                int originalHResult = enumerationError.GetInt32();
                throw new System.Runtime.InteropServices.COMException(
                    originalHResult == unchecked((int)0x80070005)
                        ? "Windows denied camera access. Check camera permissions and whether another application is using it."
                        : "Windows could not enumerate the selected camera's formats.", originalHResult);
            }
            string cameraId = devices[0].GetProperty("id").GetString()!;
            var formats = devices[0].GetProperty("formats").EnumerateArray().Where(format => format.GetProperty("supportedInput").GetBoolean())
                .Select(format => new RecordingCameraOptions(cameraId,format.GetProperty("width").GetInt32(),format.GetProperty("height").GetInt32(),
                    format.GetProperty("fpsNum").GetInt32(),format.GetProperty("fpsDen").GetInt32(),format.GetProperty("subtype").GetString()!)).ToArray();
            _camera = request.Camera is { } requestedCamera ? requestedCamera with { DeviceId = cameraId }
                : formats.Where(format => format.Width <= 1920 && format.Height <= 1080)
                    .OrderByDescending(format => (long)format.Width * format.Height)
                    .ThenBy(format => Math.Abs((double)format.FpsNum / format.FpsDen - request.RuntimeOptions.RecordingFps))
                    .FirstOrDefault();
            if (_camera is null || !formats.Contains(_camera)) throw new NotSupportedException("The explicit camera format is no longer advertised by this device.");
        }
        ValidateEndpoint(request.Targets.MicDeviceId, false);
        ValidateEndpoint(request.Targets.LoopbackDeviceId, true);
        // Native preparation owns the complete ordered selection policy and
        // preserves the first causal error. No managed retry may hide it.
        var mode = request.Targets.AnyVideo ? request.ExecutionMode : RecordingExecutionMode.FullSoftware;
        await PrepareRouteAsync(mode, id).ConfigureAwait(false);
    }
    private void ValidateEndpoint(string? id, bool loopback)
    {
        if (id is null) return;
        using var sources = _engine!.GetAudioSources(loopback);
        if (!sources.RootElement.GetProperty("devices").EnumerateArray().Any(item => item.GetProperty("id").GetString() == id))
            throw new NotSupportedException("The selected audio device is no longer available: " + id);
    }
    private async Task PrepareRouteAsync(RecordingExecutionMode mode, string? monitor)
    {
        bool gpu = mode != RecordingExecutionMode.FullSoftware;
        string nativeMode = NativeRecordingSelection.ModeName(mode);
        var bounds = request.Crop ?? request.Targets.Screen?.Bounds;
        var video = request.Video ?? RecordingQualityPresets.Video(request.QualityPreset ?? RecordingQualityPreset.High,
            request.RuntimeOptions.RecordingFps, bounds?.Width ?? _camera?.Width ?? 1280, bounds?.Height ?? _camera?.Height ?? 720);
        if (!request.Targets.CaptureScreen && _camera is { } input && request.Video is null)
            video = video with { Width = Math.Min(input.Width, video.Width), Height = Math.Min(input.Height, video.Height) };
        var capture = new Dictionary<string, object?>
        {
            ["version"] = 1, ["mode"] = nativeMode, ["monitor"] = monitor, ["cursor"] = true,
            ["slots"] = gpu ? 6 : 3, ["fpsNum"] = request.RuntimeOptions.RecordingFps, ["fpsDen"] = 1,
            ["output"] = new { width = video.Width, height = video.Height, oddPolicy = "PadEven" }
        };
        if (request.Crop is { } crop) capture["crop"] = new { x = crop.X, y = crop.Y, width = crop.Width, height = crop.Height, space = "global-physical" };
        var audio = new List<object>();
        object AudioSource(string kind, string endpointId, string path, float gain) => _supportsAudioGain
            ? new { kind, endpointId, path, gain } : (object)new { kind, endpointId, path };
        if (request.Targets.MicDeviceId is { } mic) audio.Add(AudioSource("microphone", mic, Path.Combine(request.OutputDirectory, "mic.wav"), request.MicrophoneGain));
        if (request.Targets.LoopbackDeviceId is { } loopback) audio.Add(AudioSource("loopback", loopback, Path.Combine(request.OutputDirectory, "system.wav"), request.SystemAudioGain));
        var controls = new Dictionary<string, object?>
        {
            ["version"] = 1, ["interactive"] = true, ["capture"] = capture,
            ["encoder"] = new { backend = NativeRecordingSelection.EncoderName(mode), bitrateBps = video.BitrateBps, maxPendingFrames = 16,
                quality = (request.QualityPreset ?? (request.Video is null ? RecordingQualityPreset.High : RecordingQualityPreset.Balanced)).ToString() },
            ["outputPath"] = Path.Combine(request.OutputDirectory, "screen.mkv")
        };
        if (!request.Targets.CaptureScreen) { controls.Remove("capture"); controls["outputPath"] = Path.Combine(request.OutputDirectory,"webcam.mkv"); }
        if (_camera is { } camera)
        {
            controls["camera"] = NativeRecordingWorkspace.CameraPlan(camera);
            var cameraVideo = request.Video is { } explicitVideo
                ? explicitVideo with { Width = Math.Min(camera.Width, explicitVideo.Width), Height = Math.Min(camera.Height, explicitVideo.Height) }
                : RecordingQualityPresets.Video(request.QualityPreset ?? RecordingQualityPreset.High,
                    request.RuntimeOptions.RecordingFps, camera.Width, camera.Height);
            var output = new { width = request.Targets.CaptureScreen ? cameraVideo.Width : video.Width,
                height = request.Targets.CaptureScreen ? cameraVideo.Height : video.Height,
                fpsNum=request.RuntimeOptions.RecordingFps, fpsDen=1, slots=gpu ? 6 : 3 };
            if (request.Targets.CaptureScreen) { controls["cameraOutputPath"] = Path.Combine(request.OutputDirectory,"webcam.mkv"); controls["cameraOutput"] = output; }
            else { controls["mode"] = nativeMode; controls["output"] = output; }
        }
        if (!request.Targets.AnyVideo)
        { controls.Remove("outputPath"); controls.Remove("encoder"); controls["manifestPath"] = Path.Combine(request.OutputDirectory,"audio.native.json"); }
        // Presence of the native audio field means mandatory audio was requested.
        if (audio.Count != 0) controls["audio"] = audio;
        if (request.Targets.AnyVideo && (request.Scene is not null || request.OutputMode != RecordingOutputMode.SeparateSources))
        {
            _scene = request.Scene ?? RecordingScenes.Create(request.Targets.CaptureScreen && request.Targets.CaptureWebcam ? RecordingSceneLayout.PictureInPicture :
                request.Targets.CaptureScreen ? RecordingSceneLayout.Screen : RecordingSceneLayout.Camera, video.Width, video.Height,
                request.Targets.CaptureScreen, request.Targets.CaptureWebcam) with { FpsNum = request.RuntimeOptions.RecordingFps };
            controls["scene"] = JsonSerializer.SerializeToElement(_scene, SceneJson);
            controls["recordingMode"] = request.OutputMode.ToString();
            controls["programOutputPath"] = Path.Combine(request.OutputDirectory,"program.mkv");
            controls["encoderBudget"] = 3;
        }
        var plan = JsonSerializer.SerializeToElement(controls);
        _session = _engine!.CreateSession(plan);
        Check(await _session.PrepareAsync(_startupCancellation.Token).ConfigureAwait(false));
        using var snapshot = _session.GetSnapshot();
        if (SnapshotError(snapshot.RootElement) is { } error) throw error;
        var effectiveMode = request.Targets.AnyVideo ? NativeRecordingSelection.EffectiveMode(snapshot.RootElement) : RecordingExecutionMode.FullSoftware;
        string reason = request.Targets.AnyVideo
            ? $"Native preparation selected {NativeRecordingSelection.ModeName(effectiveMode)} before Start; encoder choices and attempts are recorded in native diagnostics."
            : "Native audio-only WASAPI session; video/GPU stages are not applicable";
        Selection = new(request.ExecutionMode, effectiveMode, request.Targets.AnyVideo ? video : null,
            snapshot.RootElement.GetProperty("effectiveStages").GetRawText(), reason);
        Workspace?.RecordingPrepared(_session, request.Targets.AnyVideo && effectiveMode != RecordingExecutionMode.FullSoftware);
        log.Write($"[NativeRecording] approved={JsonSerializer.Serialize(Selection)} session={_session.SessionId} build={_engine.Build.Build}");
    }
    public async Task UpdateSceneAsync(RecordingScene scene, CancellationToken cancellationToken = default)
    {
        await _control.WaitAsync(cancellationToken).ConfigureAwait(false);
        try
        {
            EnsureUsable();
            if (_scene is null || State is not (RecordingState.Recording or RecordingState.Paused)) throw new InvalidOperationException("An active native scene is required.");
            await _session!.UpdateSceneAsync(JsonSerializer.SerializeToElement(scene, SceneJson), cancellationToken).ConfigureAwait(false);
            _scene = scene;
        }
        finally { _control.Release(); }
    }
    private async Task ObserveAsync()
    {
        try
        {
            using var timer = new PeriodicTimer(TimeSpan.FromMilliseconds(200));
            while (await timer.WaitForNextTickAsync(_observation.Token).ConfigureAwait(false))
            {
                using var snapshot = _session!.GetSnapshot();
                ObserveSnapshot(snapshot.RootElement);
                if (SnapshotError(snapshot.RootElement) is { } error) Fail(error);
            }
        }
        catch (OperationCanceledException) when (_observation.IsCancellationRequested) { }
        catch (Exception error) when (!_observation.IsCancellationRequested) { Fail(error); }
    }
    private void ObserveSnapshot(JsonElement root)
    {
        long? frames = root.GetProperty("metrics").TryGetProperty("framesSubmitted", out var value) ? value.GetInt64() : null;
        Volatile.Write(ref _statistics, new(TimeSpan.FromTicks(root.GetProperty("activeTimeHns").GetInt64()), frames, Statistics.FinalizationDuration));
    }
    private async Task CommandAsync(RecordingState transient, RecordingState completed, Func<NativeSession, Task<NativeCommandCompletion>> command, params RecordingState[] allowed)
    {
        await _control.WaitAsync().ConfigureAwait(false);
        try
        {
            EnsureUsable();
            if (State == completed) return;
            if (!allowed.Contains(State)) throw new InvalidOperationException($"Cannot enter {transient} from {State}.");
            Publish(transient);
            Check(await command(_session!).ConfigureAwait(false));
            using var snapshot = _session!.GetSnapshot();
            ObserveSnapshot(snapshot.RootElement);
            if (SnapshotError(snapshot.RootElement) is { } error) throw error;
            if (transient == RecordingState.Starting) { _started = true; _startedUtc = DateTimeOffset.UtcNow; }
            Publish(completed);
        }
        catch (Exception error) { Fail(error); throw; }
        finally { _control.Release(); }
    }
    public Task StartAsync() => CommandAsync(RecordingState.Starting, RecordingState.Recording,
        s => s.StartAsync(_startupCancellation.Token), RecordingState.Ready);
    public Task PauseAsync() => CommandAsync(RecordingState.Pausing, RecordingState.Paused, s => s.PauseAsync(), RecordingState.Recording);
    public Task ResumeAsync() => CommandAsync(RecordingState.Resuming, RecordingState.Recording, s => s.ResumeAsync(), RecordingState.Paused);
    public Task StopAsync() => StopCoreAsync(null);
    private async Task StopCoreAsync(IProgress<RecordingSaveProgress>? progress)
    {
        await _control.WaitAsync().ConfigureAwait(false);
        try
        {
            ObjectDisposedException.ThrowIf(Volatile.Read(ref _disposing) != 0, this);
            if (_finalSnapshot is not null || Result is not null) return;
            Publish(RecordingState.Stopping);
            await CloseSessionAsync(progress: progress).ConfigureAwait(false);
            Publish(RecordingState.Finalizing);
        }
        catch (Exception error) { Fail(error); throw; }
        finally { _control.Release(); }
    }
    private async Task CloseSessionAsync(bool endObservation = true, IProgress<RecordingSaveProgress>? progress = null)
    {
        if (endObservation)
        {
            _observation.Cancel();
            await _observer.ConfigureAwait(false);
        }
        if (_session is null || _finalSnapshot is not null) return;
        Workspace?.RecordingEnded(_session);
        var elapsed = Stopwatch.StartNew();
        var closing = _session.CloseAsync();
        // Accepted native writes must complete before ownership is released.
        // Keep presentation alive during a slow disk without cancelling valid
        // media or claiming a percentage that the storage API cannot provide.
        while (progress is not null && !closing.IsCompleted)
        {
            if (await Task.WhenAny(closing, Task.Delay(1000)).ConfigureAwait(false) == closing) break;
            bool writing = false;
            string message = "Closing recording and pending writes...";
            try
            {
                using var pending = _session.GetSnapshot();
                if (pending.RootElement.TryGetProperty("shutdownStage", out var stage))
                    message = stage.GetString() switch
                    {
                        "Sources" => "Closing recording devices...",
                        "Inflight" => "Finishing captured frames...",
                        "EncoderEos" => "Finishing video encoding...",
                        "Packets" => "Draining encoded frames...",
                        "Output" => "Finishing recording files...",
                        "Resources" => "Releasing recording devices...",
                        _ => message
                    };
                if (pending.RootElement.TryGetProperty("journalDiagnostics", out var journal) &&
                    journal.TryGetProperty("durableWriter", out var writer) && writer.ValueKind == JsonValueKind.Object)
                    writing = writer.GetProperty("active").GetBoolean() || writer.GetProperty("pending").GetBoolean();
            }
            catch (ObjectDisposedException) when (closing.IsCompleted) { break; }
            progress.Report(new()
            {
                Title = "Saving Recording...",
                Message = writing
                    ? $"Waiting for Windows to finish writing... ({elapsed.Elapsed.TotalSeconds:0}s)"
                    : $"{message} ({elapsed.Elapsed.TotalSeconds:0}s)",
                IsIndeterminate = true, CurrentStep = 1, TotalSteps = 8
            });
        }
        using var final = await closing.ConfigureAwait(false);
        _finalSnapshot = final.RootElement.GetRawText();
        _stoppedUtc = DateTimeOffset.UtcNow;
        ObserveSnapshot(final.RootElement);
        if (SnapshotError(final.RootElement) is { } error && _started) Fail(error);
        if (!_started && _failure is { } preparationError)
        {
            try
            {
                NativeSessionManifestReader.WriteAtomic(Path.Combine(request.OutputDirectory, "recording-startup-error.json"),
                    new { version = 1, status = "Failed", started = false, projectComplete = false,
                        error = UserError.Message(preparationError), nativeSnapshot = final.RootElement });
            }
            catch (Exception diagnosticError) { log.Write("[NativeRecording] Startup diagnostics: " + UserError.Details(diagnosticError)); }
        }
    }
    public async Task<string> StopAndFinalizeAsync(IProgress<RecordingSaveProgress>? progress = null)
    {
        await StopCoreAsync(progress).ConfigureAwait(false);
        await _control.WaitAsync().ConfigureAwait(false);
        SessionManifest? recoveryManifest = null;
        try
        {
            EnsureUsable();
            if (Result is { } existing) return existing.PrimaryAssetPath;
            if (!_started || _finalSnapshot is null) throw new InvalidOperationException("No started native recording is available to finalize.");
            var finalization = Stopwatch.StartNew();
            using var snapshot = JsonDocument.Parse(_finalSnapshot);
            if (!snapshot.RootElement.GetProperty("mediaComplete").GetBoolean()) throw new InvalidDataException("Native media was not finalized; partial assets are preserved.");
            Report(progress, "Reading captured track metadata...", 70);
            bool hasProgram = request.Targets.AnyVideo && request.OutputMode != RecordingOutputMode.SeparateSources;
            bool hasRaw = request.OutputMode != RecordingOutputMode.Program;
            // Recording saves its original tracks. Conversion and composition
            // belong to the explicit Export workflow, never to Stop/Save.
            string primary = Path.Combine(request.OutputDirectory, hasProgram ? "program.mkv" : request.Targets.CaptureScreen ? "screen.mkv"
                : request.Targets.CaptureWebcam ? "webcam.mkv" : request.Targets.CaptureMic ? "mic.wav" : "system.wav");
            string nativeManifest = request.Targets.AnyVideo ? primary + ".manifest.json" : Path.Combine(request.OutputDirectory,"audio.native.json");
            var manifest = NativeSessionManifestReader.Read(nativeManifest, request.OutputDirectory, request.Targets,
                camera: !hasProgram && !request.Targets.CaptureScreen && request.Targets.CaptureWebcam, program: hasProgram);
            recoveryManifest = manifest;
            if (hasProgram && hasRaw && request.Targets.CaptureScreen)
            {
                var raw = NativeSessionManifestReader.Read(Path.Combine(request.OutputDirectory,"screen.mkv.manifest.json"),request.OutputDirectory,
                    new RecordingTargets(request.Targets.Screen,null,null,null));
                if (Math.Abs(raw.DurationMs!.Value-manifest.DurationMs!.Value)>34) throw new InvalidDataException("Raw screen and program do not share the finalized cut.");
                manifest.HasScreen=true; manifest.Screen=raw.Screen;
            }
            if (hasRaw && request.Targets.CaptureWebcam)
            {
                string cameraManifest = Path.Combine(request.OutputDirectory,"webcam.mkv.manifest.json");
                if (hasProgram || request.Targets.CaptureScreen)
                {
                    var independent = NativeSessionManifestReader.Read(cameraManifest,request.OutputDirectory,
                        new RecordingTargets(null,request.Targets.WebcamName,null,null),camera:true);
                    if (Math.Abs(independent.DurationMs!.Value-manifest.DurationMs!.Value) > 34)
                        throw new InvalidDataException("Independent camera and program duration do not share the finalized cut.");
                    manifest.HasWebcam = true; manifest.Webcam = independent.Webcam;
                }
            }
            manifest.RecordingMode=request.OutputMode.ToString(); manifest.Scene=_scene;
            if (snapshot.RootElement.GetProperty("metrics").TryGetProperty("sharedScene",out var sharedScene)) manifest.SceneHistory=sharedScene.GetProperty("history").Clone();
            Report(progress, "Persisting recording metadata...", 85);
            manifest.CreatedAt = _created; manifest.StartedAtUtc = _startedUtc; manifest.StoppedAtUtc = _stoppedUtc;
            manifest.NativeSelection = Selection; manifest.PrimaryAsset = Path.GetFileName(primary);
            ProjectManifestStore.UpgradeNewRecording(manifest,request.OutputDirectory,"libnessstudio/"+_engine!.Build.Build);
            string? audioNotice = AudioRecoveryNotice(snapshot.RootElement);
            ProjectMetadataStore.SetDocument(manifest, "recording-native.json", JsonSerializer.SerializeToElement(new
            {
                version = 1, backend = Capabilities.Backend, build = _engine!.Build, selection = Selection,
                request = new { request.ExecutionMode, request.QualityPreset, request.OutputMode, scene = _scene, request.Crop, request.Targets.Screen, camera = _camera, request.Targets.MicDeviceId, request.Targets.LoopbackDeviceId },
                automaticExport = false, finalizationNotice = audioNotice, snapshot = snapshot.RootElement
            }));
            manifest.Metadata!.Registration = JsonSerializer.SerializeToElement(new { version = 1, projectId = manifest.ProjectId,
                registered = false, recoverable = true, sourceAssetsPreserved = true, error = "Finalized native project is awaiting library registration." });
            manifest.Metadata.Recovery = JsonSerializer.SerializeToElement(new { version = 1, resolved = true, projectId = manifest.ProjectId, sourceAssetsPreserved = true });
            ProjectMetadataStore.Consolidate(request.OutputDirectory, manifest);
            Volatile.Write(ref _statistics, new(TimeSpan.FromMilliseconds(manifest.DurationMs!.Value), manifest.HasProgram ? manifest.Program!.FrameCount : manifest.HasScreen ? manifest.Screen.FrameCount : manifest.HasWebcam ? manifest.Webcam.FrameCount : null, finalization.Elapsed));
            Result = new(request.OutputDirectory, primary, Path.Combine(request.OutputDirectory, "manifest.json"), Capabilities.Backend, Statistics, Selection,
                manifest.HasWebcam ? Path.Combine(request.OutputDirectory,manifest.Webcam.File) : null) { FinalizationNotice = audioNotice };
            Publish(RecordingState.Completed);
            return primary;
        }
        catch (Exception error)
        {
            Fail(error);
            if (recoveryManifest is not null)
            {
                try
                {
                    if (recoveryManifest.Version != 3)
                    {
                        recoveryManifest.PrimaryAsset ??= recoveryManifest.Program?.File ?? recoveryManifest.Screen?.File ?? recoveryManifest.Webcam?.File ?? recoveryManifest.Mic?.File ?? recoveryManifest.System?.File;
                        ProjectManifestStore.UpgradeNewRecording(recoveryManifest,request.OutputDirectory,"libnessstudio/"+_engine!.Build.Build);
                    }
                    ProjectRecoveryService.Stage(request.OutputDirectory,recoveryManifest,UserError.Message(error));
                }
                catch(Exception recoveryError){log.Write("[NativeRecording] Recovery metadata: "+recoveryError);}
            }
            if (_started && _finalSnapshot is not null && Result is null)
            {
                string partial = Path.Combine(request.OutputDirectory,"recording-incomplete.json");
                try
                {
                    if (!File.Exists(partial)) NativeSessionManifestReader.WriteAtomic(partial,new { version=1,status="Failed",
                        projectComplete=false,sourceAssetsPreserved=true,error=UserError.Message(error),nativeSnapshot=JsonSerializer.Deserialize<JsonElement>(_finalSnapshot) });
                }
                catch (Exception metadataError) { log.Write("[NativeRecording] Incomplete metadata: " + metadataError); }
            }
            throw;
        }
        finally { _control.Release(); }
    }
    private static string? AudioRecoveryNotice(JsonElement snapshot)
    {
        // Only silence actually inserted into active output is user-visible.
        // Sub-100ms recoveries and gaps discarded by a pause remain diagnostic.
        if (!snapshot.GetProperty("metrics").TryGetProperty("audio", out var tracks) || tracks.ValueKind != JsonValueKind.Array)
            return null;
        return tracks.EnumerateArray().Any(track => track.TryGetProperty("recoveredAudioGaps", out var gaps) &&
            gaps.GetProperty("insertedSilentOutputFrames").GetUInt64() >= 4800)
            ? "An audio device interruption affected part of this recording. It was recovered with silence and your recording was saved. Details are available in the project diagnostics."
            : null;
    }
    private static void Report(IProgress<RecordingSaveProgress>? progress, string message, int percent) =>
        progress?.Report(new() { Title = "Saving Recording...", Message = message, Percent = percent, CurrentStep = 6, TotalSteps = 8 });

    public ValueTask DisposeAsync()
    {
        lock (_releaseGate)
        {
            Interlocked.Exchange(ref _disposing, 1);
            return new(_release ??= ReleaseAsync());
        }
    }
    private async Task ReleaseAsync()
    {
        await _control.WaitAsync().ConfigureAwait(false);
        try
        {
            await CloseSessionAsync().ConfigureAwait(false);
            // CloseAsync has observed every native worker's completion before
            // either owned handle or the engine context may be released.
            _engine?.Dispose(); _engine = null;
            _observation.Dispose(); _startupCancellation.Dispose(); Publish(RecordingState.Disposed); StateChanged = null;
        }
        finally { _control.Release(); }
    }
    public void Dispose() => _ = ObserveReleaseAsync();
    private async Task ObserveReleaseAsync()
    {
        try { await DisposeAsync().ConfigureAwait(false); }
        catch (Exception error) { log.Write("[NativeRecording] Cleanup failed: " + UserError.Details(error)); }
    }
}

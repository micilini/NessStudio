using System.Diagnostics;
using System.Text.Json;
using NessStudio.Diagnostics;
using NessStudio.Interop.Native;
using NessStudio.Models;

namespace NessStudio.Recording.Engines;

/// <summary>Composes native control, bounded observation and final project metadata; never owns media buffers.</summary>
internal sealed class NativeRecordingService(RecordingRequest request, string applicationDirectory, IDiagnosticLog log) : IRecordingService
{
    internal static readonly string[] OwnedAssetNames = ["program.mkv", "program.mkv.manifest.json", "program.mkv.manifest.pending", "program.mkv.partial.manifest.json", "program.mkv.partial.manifest.pending", "screen.mkv", "screen.mkv.manifest.json", "screen.mkv.manifest.pending",
        "screen.mkv.partial.manifest.json", "screen.mkv.partial.manifest.pending", "mic.wav", "system.wav", "final.mkv",
        "manifest.json", "manifest.json.pending", "recording-native.json", "recording-native.json.pending", "native-export.json", "native-export.json.pending",
        "webcam.mkv", "webcam.mkv.manifest.json", "webcam.mkv.manifest.pending", "webcam.mkv.partial.manifest.json", "webcam.mp4",
        "webcam.mp4.partial.mp4", "webcam.mp4.export.json", "audio.native.json", "audio.native.json.pending", "native-camera-export.json",
        "native-camera-export.json.pending", "recording-incomplete.json", "recording-incomplete.json.pending", "session.manifest.json", "project-recovery.json", "project-registration.json"];
    internal NativeRecordingWorkspace? Workspace { get; set; }
    private RecordingCameraOptions? _camera;
    private RecordingScene? _scene;
    private static readonly JsonSerializerOptions SceneJson = new() { PropertyNamingPolicy = JsonNamingPolicy.CamelCase };
    private readonly SemaphoreSlim _control = new(1, 1);
    private readonly object _releaseGate = new();
    private readonly CancellationTokenSource _observation = new();
    private NativeEngine? _engine;
    private NativeSession? _session;
    private Task _observer = Task.CompletedTask;
    private Task? _release;
    private Exception? _failure;
    private int _state, _disposing;
    private bool _started;
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
            catch (Exception observerError) { log.Write("[NativeRecording] Observer: " + observerError.Message); }
    }
    private void Fail(Exception error)
    {
        if (Interlocked.CompareExchange(ref _failure, error, null) is null)
        {
            log.Write("[NativeRecording] Failed: " + error);
            Publish(RecordingState.Failed, error);
        }
    }
    private void EnsureUsable()
    {
        ObjectDisposedException.ThrowIf(Volatile.Read(ref _disposing) != 0, this);
        if (_failure is { } error) throw new InvalidOperationException("The native recording failed: " + error.Message, error);
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

    public async Task PrepareAsync()
    {
        await _control.WaitAsync().ConfigureAwait(false);
        try
        {
            EnsureUsable();
            if (State == RecordingState.Ready) return;
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
        if ((request.Targets.AnyAudio || request.Targets.CaptureWebcam) && !File.Exists(Path.Combine(applicationDirectory, "Native", "FFmpeg", "ffmpeg.exe")))
            throw new FileNotFoundException("Screen/audio recording requires the packaged FFmpeg export dependency.");
        _engine = NativeEngine.Open(applicationDirectory, "NessStudio.MVVM");
        if (_engine.Build.AbiMinor < 8) throw new NotSupportedException("Native recording journals and recovery require libnessstudio ABI 1.8 or newer.");
        using var capabilities = _engine.GetCapabilities();
        var declared = capabilities.RootElement.GetProperty("declaredCapabilities");
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
                : formats.FirstOrDefault(format => format.Width == 1280 && format.Height == 720 && format.FpsNum == 30 && format.FpsDen == 1)
                ?? formats.FirstOrDefault(format => format.Width <= 1280 && format.Height <= 720);
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
        bool fullHdDefault = mode is RecordingExecutionMode.RequireGpuResidentHardware or RecordingExecutionMode.GpuWithSoftwareEncoding;
        string nativeMode = NativeRecordingSelection.ModeName(mode);
        var video = request.Video ?? new RecordingVideoOptions(fullHdDefault && request.Targets.CaptureScreen ? 1920 : 1280,
            fullHdDefault && request.Targets.CaptureScreen ? 1080 : 720, fullHdDefault ? 12000000 : 6000000);
        if (!request.Targets.CaptureScreen && _camera is { } input && request.Video is null)
            video = video with { Width = Math.Min(input.Width,fullHdDefault ? 1920 : 1280), Height = Math.Min(input.Height,fullHdDefault ? 1080 : 720) };
        var capture = new Dictionary<string, object?>
        {
            ["version"] = 1, ["mode"] = nativeMode, ["monitor"] = monitor, ["cursor"] = true,
            ["slots"] = gpu ? 6 : 3, ["fpsNum"] = request.RuntimeOptions.RecordingFps, ["fpsDen"] = 1,
            ["output"] = new { width = video.Width, height = video.Height, oddPolicy = "PadEven" }
        };
        if (request.Crop is { } crop) capture["crop"] = new { x = crop.X, y = crop.Y, width = crop.Width, height = crop.Height, space = "global-physical" };
        var audio = new List<object>();
        if (request.Targets.MicDeviceId is { } mic) audio.Add(new { kind = "microphone", endpointId = mic, path = Path.Combine(request.OutputDirectory, "mic.wav") });
        if (request.Targets.LoopbackDeviceId is { } loopback) audio.Add(new { kind = "loopback", endpointId = loopback, path = Path.Combine(request.OutputDirectory, "system.wav") });
        var controls = new Dictionary<string, object?>
        {
            ["version"] = 1, ["interactive"] = true, ["capture"] = capture,
            ["encoder"] = new { backend = NativeRecordingSelection.EncoderName(mode), bitrateBps = video.BitrateBps, maxPendingFrames = 16 },
            ["outputPath"] = Path.Combine(request.OutputDirectory, "screen.mkv")
        };
        if (!request.Targets.CaptureScreen) { controls.Remove("capture"); controls["outputPath"] = Path.Combine(request.OutputDirectory,"webcam.mkv"); }
        if (_camera is { } camera)
        {
            controls["camera"] = NativeRecordingWorkspace.CameraPlan(camera);
            var output = new { width = request.Targets.CaptureScreen ? Math.Min(camera.Width,request.Video is null?1280:video.Width) : video.Width,
                height = request.Targets.CaptureScreen ? Math.Min(camera.Height,request.Video is null?720:video.Height) : video.Height,
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
        Check(await _session.PrepareAsync().ConfigureAwait(false));
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
    public Task StartAsync() => CommandAsync(RecordingState.Starting, RecordingState.Recording, s => s.StartAsync(), RecordingState.Ready);
    public Task PauseAsync() => CommandAsync(RecordingState.Pausing, RecordingState.Paused, s => s.PauseAsync(), RecordingState.Recording);
    public Task ResumeAsync() => CommandAsync(RecordingState.Resuming, RecordingState.Recording, s => s.ResumeAsync(), RecordingState.Paused);
    public async Task StopAsync()
    {
        await _control.WaitAsync().ConfigureAwait(false);
        try
        {
            ObjectDisposedException.ThrowIf(Volatile.Read(ref _disposing) != 0, this);
            if (_finalSnapshot is not null || Result is not null) return;
            Publish(RecordingState.Stopping);
            await CloseSessionAsync().ConfigureAwait(false);
            Publish(RecordingState.Finalizing);
        }
        catch (Exception error) { Fail(error); throw; }
        finally { _control.Release(); }
    }
    private async Task CloseSessionAsync(bool endObservation = true)
    {
        if (endObservation)
        {
            _observation.Cancel();
            await _observer.ConfigureAwait(false);
        }
        if (_session is null || _finalSnapshot is not null) return;
        Workspace?.RecordingEnded(_session);
        using var final = await _session.CloseAsync().ConfigureAwait(false);
        _finalSnapshot = final.RootElement.GetRawText();
        _stoppedUtc = DateTimeOffset.UtcNow;
        ObserveSnapshot(final.RootElement);
        if (SnapshotError(final.RootElement) is { } error && _started) Fail(error);
    }
    public async Task<string> StopAndFinalizeAsync(IProgress<RecordingSaveProgress>? progress = null)
    {
        await StopAsync().ConfigureAwait(false);
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
            Report(progress, "Closing native media...", 70);
            bool hasProgram = request.Targets.AnyVideo && request.OutputMode != RecordingOutputMode.SeparateSources;
            bool hasRaw = request.OutputMode != RecordingOutputMode.Program;
            string primary = Path.Combine(request.OutputDirectory, hasProgram ? "program.mkv" : request.Targets.CaptureScreen ? "screen.mkv" : request.Targets.CaptureWebcam ? "webcam.mkv" : "final.mkv");
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
                string effectiveCamera = await RunExportAsync("camera-remux",cameraManifest,Path.Combine(request.OutputDirectory,"webcam.mp4"),"native-camera-export.json").ConfigureAwait(false);
                manifest.Webcam.File = Path.GetFileName(effectiveCamera); manifest.Webcam.ContainerKind = "mp4";
                if (!hasProgram && !request.Targets.CaptureScreen) primary = effectiveCamera;
            }
            manifest.RecordingMode=request.OutputMode.ToString(); manifest.Scene=_scene;
            if (snapshot.RootElement.GetProperty("metrics").TryGetProperty("sharedScene",out var sharedScene)) manifest.SceneHistory=sharedScene.GetProperty("history").Clone();
            if (request.Targets.AnyAudio)
            {
                Report(progress, "Combining screen and audio...", 75);
                await RunExportAsync(request.Targets.AnyVideo ? "av-basic" : "audio-basic", nativeManifest,
                    Path.Combine(request.OutputDirectory,"final.mkv"),"native-export.json").ConfigureAwait(false);
                primary = Path.Combine(request.OutputDirectory, "final.mkv");
            }
            Report(progress, "Persisting recording metadata...", 85);
            manifest.CreatedAt = _created; manifest.StartedAtUtc = _startedUtc; manifest.StoppedAtUtc = _stoppedUtc;
            manifest.NativeSelection = Selection; manifest.PrimaryAsset = Path.GetFileName(primary);
            ProjectManifestStore.UpgradeNewRecording(manifest,request.OutputDirectory,"libnessstudio/"+_engine!.Build.Build);
            ProjectRecoveryService.Stage(request.OutputDirectory,manifest);
            NativeSessionManifestReader.WriteAtomic(Path.Combine(request.OutputDirectory, "recording-native.json"), new
            {
                version = 1, backend = Capabilities.Backend, build = _engine!.Build, selection = Selection,
                request = new { request.ExecutionMode, request.OutputMode, scene = _scene, request.Crop, request.Targets.Screen, camera = _camera, request.Targets.MicDeviceId, request.Targets.LoopbackDeviceId },
                snapshot = snapshot.RootElement
            });
            ProjectManifestStore.WriteAliases(request.OutputDirectory, manifest);
            ProjectManifestStore.RecordRegistration(request.OutputDirectory,manifest.ProjectId!,false,"Finalized native project is awaiting library registration.");
            ProjectRecoveryService.Resolve(request.OutputDirectory,manifest.ProjectId!);
            Volatile.Write(ref _statistics, new(TimeSpan.FromMilliseconds(manifest.DurationMs!.Value), manifest.HasProgram ? manifest.Program!.FrameCount : manifest.HasScreen ? manifest.Screen.FrameCount : manifest.HasWebcam ? manifest.Webcam.FrameCount : null, finalization.Elapsed));
            Result = new(request.OutputDirectory, primary, Path.Combine(request.OutputDirectory, "manifest.json"), Capabilities.Backend, Statistics, Selection,
                manifest.HasWebcam ? Path.Combine(request.OutputDirectory,manifest.Webcam.File) : null);
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
                    ProjectRecoveryService.Stage(request.OutputDirectory,recoveryManifest,error.Message);
                }
                catch(Exception recoveryError){log.Write("[NativeRecording] Recovery metadata: "+recoveryError);}
            }
            if (_started && _finalSnapshot is not null && Result is null)
            {
                string partial = Path.Combine(request.OutputDirectory,"recording-incomplete.json");
                try
                {
                    if (!File.Exists(partial)) NativeSessionManifestReader.WriteAtomic(partial,new { version=1,status="Failed",
                        projectComplete=false,sourceAssetsPreserved=true,error=error.Message,nativeSnapshot=JsonSerializer.Deserialize<JsonElement>(_finalSnapshot) });
                }
                catch (Exception metadataError) { log.Write("[NativeRecording] Incomplete metadata: " + metadataError); }
            }
            throw;
        }
        finally { _control.Release(); }
    }
    private async Task<string> RunExportAsync(string profile,string nativeManifest,string output,string receipt)
    {
        await using var job = _engine!.CreateJob(JsonSerializer.SerializeToElement(new { version=1,type="export",profile,
            sessionManifestPath=nativeManifest,ffmpegPath=Path.Combine(applicationDirectory,"Native","FFmpeg","ffmpeg.exe"),outputPath=output }));
        using var result = await job.RunAsync().ConfigureAwait(false);
        NativeSessionManifestReader.WriteAtomic(Path.Combine(request.OutputDirectory,receipt),result.RootElement);
        if (result.RootElement.GetProperty("state").GetInt32()!=105) throw new InvalidDataException("Native " + profile + " failed: " + result.RootElement.GetRawText());
        string effective = Path.GetFullPath(result.RootElement.GetProperty("outputPath").GetString()!);
        if (!string.Equals(effective,output,StringComparison.OrdinalIgnoreCase) || !File.Exists(effective)) throw new InvalidDataException("Native export returned an unexpected finalized path.");
        return effective;
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
            _observation.Dispose(); Publish(RecordingState.Disposed); StateChanged = null;
        }
        finally { _control.Release(); }
    }
    public void Dispose() => _ = ObserveReleaseAsync();
    private async Task ObserveReleaseAsync()
    {
        try { await DisposeAsync().ConfigureAwait(false); }
        catch (Exception error) { log.Write("[NativeRecording] Cleanup failed: " + error); }
    }
}

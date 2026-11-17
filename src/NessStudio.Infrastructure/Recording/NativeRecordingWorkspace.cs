using System.Text.Json;
using NessStudio.Diagnostics;
using NessStudio.Interop.Native;
using NessStudio.Models;
using NessStudio.Recording.Engines;

namespace NessStudio.Recording;

/// <summary>Serializes idle-preview ownership and hands presentation consumers to one active recording per window.</summary>
public sealed class NativeRecordingWorkspace(string applicationDirectory, IDiagnosticLog? diagnosticLog = null) : IAsyncDisposable
{
    private readonly SemaphoreSlim _control = new(1, 1);
    private readonly object _gate = new();
    private readonly IDiagnosticLog _log = diagnosticLog ?? NullDiagnosticLog.Instance;
    private NativeEngine? _idleEngine;
    private NativeSession? _idleSession, _active;
    private bool _gpu;
    private volatile bool _suspended, _disposed;
    private long _revision;
    /// <summary>Occurs when presenters should detach their old consumer and bind the current source.</summary>
    public event EventHandler? PresentationChanged;
    /// <summary>Gets the most recent isolated preview error; recording errors belong to the recorder.</summary>
    public string? Error { get; private set; }
    /// <summary>Gets whether the current presentation uses the D3D9Ex bridge.</summary>
    public bool UsesGpu { get { lock (_gate) return _gpu; } }
    private void Changed()
    {
        foreach (var observer in PresentationChanged?.GetInvocationList() ?? [])
            try { ((EventHandler)observer)(this, EventArgs.Empty); }
            catch (Exception error) { _log.Write("[NativePreview] Observer: " + error.Message); }
    }
    /// <summary>Attaches only a presentation consumer; the caller never owns source/session shutdown.</summary>
    public NativePreview? Attach(ulong source)
    {
        lock (_gate)
        {
            if (_disposed || _active is null) return null;
            try { return _active.AttachPreview(source, _gpu); }
            catch (NativeEngineException error) when (error.ResultCode is -4 or -3) { return null; }
        }
    }
    /// <summary>Enumerates native cameras outside the UI thread without retaining camera sessions.</summary>
    public async Task<IReadOnlyList<RecordingCameraDescriptor>> GetCamerasAsync()
    {
        await _control.WaitAsync().ConfigureAwait(false);
        try
        {
            ObjectDisposedException.ThrowIf(_disposed,this);
            return await Task.Run(ReadCameras).ConfigureAwait(false);
        }
        finally { _control.Release(); }
    }
    private IReadOnlyList<RecordingCameraDescriptor> ReadCameras()
    {
        using var engine = NativeEngine.Open(applicationDirectory, "NessStudio.CameraCatalog");
        using var result = engine.GetCameraSources();
        return result.RootElement.GetProperty("devices").EnumerateArray().Select(device =>
        {
            string id = device.GetProperty("id").GetString()!;
            var formats = device.GetProperty("formats").EnumerateArray().Where(format => format.GetProperty("supportedInput").GetBoolean())
                .Select(format => new RecordingCameraOptions(id,format.GetProperty("width").GetInt32(),format.GetProperty("height").GetInt32(),
                    format.GetProperty("fpsNum").GetInt32(),format.GetProperty("fpsDen").GetInt32(),format.GetProperty("subtype").GetString()!)).ToArray();
            return new RecordingCameraDescriptor(id,device.GetProperty("name").GetString()!,Array.AsReadOnly(formats));
        }).ToArray();
    }
    internal static object CameraPlan(RecordingCameraOptions camera) => new { deviceId = camera.DeviceId, width = camera.Width,
        height = camera.Height, fpsNum = camera.FpsNum, fpsDen = camera.FpsDen, subtype = camera.Subtype };
    internal static string? ResolveMonitor(NativeEngine engine, MonitorDescriptor? requested)
    {
        if (requested is null) return null;
        using var screens = engine.GetScreenSources();
        foreach (var screen in screens.RootElement.GetProperty("devices").EnumerateArray())
            if (string.Equals(screen.GetProperty("name").GetString(), requested.DeviceName,StringComparison.OrdinalIgnoreCase))
            {
                var bounds = requested.Bounds;
                if (screen.GetProperty("x").GetInt32() != bounds.X || screen.GetProperty("y").GetInt32() != bounds.Y ||
                    screen.GetProperty("width").GetInt32() != bounds.Width || screen.GetProperty("height").GetInt32() != bounds.Height)
                    throw new InvalidOperationException("The display geometry changed. Select the monitor/region again.");
                return screen.GetProperty("id").GetString();
            }
        throw new NotSupportedException("The selected physical monitor is no longer available.");
    }
    /// <summary>Updates idle source selection. Concurrent requests supersede older requests before opening devices.</summary>
    public async Task UpdateIdleAsync(MonitorDescriptor? screen, RecordingCameraOptions? camera, RecordingExecutionMode mode, RecordingScene? scene = null)
    {
        long revision = Interlocked.Increment(ref _revision);
        await _control.WaitAsync().ConfigureAwait(false);
        try
        {
            if (_disposed || _suspended || revision != Volatile.Read(ref _revision)) return;
            await CloseIdleAsync().ConfigureAwait(false);
            if (screen is null && camera is null) return;
            bool gpu = mode != RecordingExecutionMode.FullSoftware;
            try { await OpenIdleAsync(screen,camera,gpu,scene).ConfigureAwait(false); }
            catch (NativeEngineException error) when (mode == RecordingExecutionMode.Automatic && error.Stage is 3 or 4 or 5 or 9 && error.ResultCode is -4 or -10)
            {
                await CloseIdleAsync().ConfigureAwait(false);
                _log.Write("[NativePreview] GPU unavailable before recording: " + error.Message);
                await OpenIdleAsync(screen,camera,false,scene).ConfigureAwait(false);
            }
            Error = null;
        }
        catch (Exception error) { Error = error.Message; _log.Write("[NativePreview] " + error); await CloseIdleAsync().ConfigureAwait(false); }
        finally { _control.Release(); Changed(); }
    }
    private async Task OpenIdleAsync(MonitorDescriptor? screen,RecordingCameraOptions? camera,bool gpu,RecordingScene? scene)
    {
        await Task.Run(() =>
        {
            _idleEngine = NativeEngine.Open(applicationDirectory,"NessStudio.IdlePreview");
            var plan = new Dictionary<string,object?> { ["version"]=1,["previewOnly"]=true,["mode"]=gpu ? "RequireGpuResidentHardware" : "FullSoftware" };
            if (screen is not null) plan["capture"] = new { version=1,mode=plan["mode"],monitor=ResolveMonitor(_idleEngine,screen),
                fpsNum=30,fpsDen=1,slots=gpu ? 6 : 3,output=new {width=640,height=360,oddPolicy="PadEven"} };
            if (camera is not null) plan["camera"] = CameraPlan(camera);
            if (scene is not null)
            {
                string unused = Path.Combine(Path.GetTempPath(),"NessStudio-preview-"+Guid.NewGuid().ToString("N"));
                plan["scene"]=JsonSerializer.SerializeToElement(scene with { Width=640, Height=360, FpsNum=30, FpsDen=1 },new JsonSerializerOptions { PropertyNamingPolicy=JsonNamingPolicy.CamelCase });
                plan["recordingMode"]="SeparateSources"; plan["outputPath"]=unused+"-screen.mkv";
                plan["cameraOutputPath"]=unused+"-camera.mkv";
                plan["encoder"]=new {backend=gpu ? "Auto" : "X264",bitrateBps=3000000};
                plan["output"]=new {width=640,height=360,fpsNum=30,fpsDen=1,slots=gpu ? 6 : 3};
                plan["cameraOutput"]=plan["output"];
            }
            _idleSession = _idleEngine.CreateSession(JsonSerializer.SerializeToElement(plan));
        }).ConfigureAwait(false);
        Check(await _idleSession!.PrepareAsync().ConfigureAwait(false));
        Check(await _idleSession.StartAsync().ConfigureAwait(false));
        lock (_gate) { _active = _idleSession; _gpu = gpu; }
    }
    /// <summary>Updates an existing idle scene without reopening its sources.</summary>
    public async Task UpdateSceneAsync(RecordingScene scene)
    {
        await _control.WaitAsync().ConfigureAwait(false);
        try
        {
            if (_disposed || _suspended || _idleSession is null) return;
            await _idleSession.UpdateSceneAsync(JsonSerializer.SerializeToElement(scene with {Width=640,Height=360,FpsNum=30,FpsDen=1},
                new JsonSerializerOptions {PropertyNamingPolicy=JsonNamingPolicy.CamelCase})).ConfigureAwait(false);
        }
        finally { _control.Release(); }
    }
    private static void Check(NativeCommandCompletion result)
    { if (result.ResultCode < 0) throw new NativeEngineException(result.ResultCode,result.Stage,result.OriginalHResult,result.Message); }
    private async Task CloseIdleAsync()
    {
        lock (_gate) { if (ReferenceEquals(_active,_idleSession)) _active = null; }
        Changed();
        try { if (_idleSession is { } previous) { _idleSession = null; using var final = await previous.CloseAsync().ConfigureAwait(false); CheckSnapshot(final.RootElement); } }
        finally { _idleEngine?.Dispose(); _idleEngine = null; }
    }
    private static void CheckSnapshot(JsonElement root)
    {
        if (root.TryGetProperty("firstError",out var error) && error.ValueKind == JsonValueKind.Object)
            throw new NativeEngineException(error.GetProperty("code").GetInt32(),error.GetProperty("stage").GetUInt32(),error.GetProperty("hresult").GetInt32(),error.GetProperty("message").GetString()!);
    }
    /// <summary>Waits for all idle sources to close before the recorder may prepare the next source owner.</summary>
    public async Task SuspendAsync()
    {
        await _control.WaitAsync().ConfigureAwait(false);
        try { _suspended = true; await CloseIdleAsync().ConfigureAwait(false); }
        finally { _control.Release(); }
    }
    /// <summary>Allows subsequent idle-selection updates after recording ownership has ended.</summary>
    public void ResumeIdle() { lock (_gate) _suspended = false; }
    internal void RecordingPrepared(NativeSession session,bool gpu)
    { lock (_gate) { if (_disposed) return; _active = session; _gpu = gpu; } Changed(); }
    internal void RecordingEnded(NativeSession session)
    { lock (_gate) { if (!ReferenceEquals(_active,session)) return; _active = null; } Changed(); }
    /// <summary>Associates one newly created native recorder with this window's presentation lifecycle.</summary>
    public void Adopt(IRecordingService service)
    { if (service is NativeRecordingService native) native.Workspace = this; }
    /// <inheritdoc />
    public async ValueTask DisposeAsync()
    {
        await _control.WaitAsync().ConfigureAwait(false);
        try { if (_disposed) return; _disposed = true; await CloseIdleAsync().ConfigureAwait(false); lock (_gate) _active = null; Changed(); PresentationChanged = null; }
        finally { _control.Release(); }
    }
}

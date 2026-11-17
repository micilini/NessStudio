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
    private sealed record IdleSelection(MonitorDescriptor? Screen, RecordingCameraOptions? Camera, RecordingExecutionMode Mode)
    {
        public bool Gpu => Mode != RecordingExecutionMode.FullSoftware;
    }
    private sealed record IdleSource(IdleSelection Selection, NativeEngine Engine, NativeSession Session, bool Gpu);
    private readonly Dictionary<ulong, IdleSource> _idleSources = new();
    private readonly Dictionary<ulong, string> _errors = new();
    // Native command tasks intentionally permit abandonment. Keep the source
    // rooted while preparation/start awaits events, before it enters the idle map.
    private IdleSource? _openingSource;
    private IdleSource? _closingSource;
    private NativeSession? _recording;
    private bool _recordingGpu;
    private volatile bool _suspended, _disposed;
    private long _revision;
    /// <summary>Occurs when presenters should detach their old consumer and bind the current source.</summary>
    public event EventHandler? PresentationChanged;
    /// <summary>Gets the most recent isolated preview error; recording errors belong to the recorder.</summary>
    public string? Error { get { lock (_gate) return _errors.Count == 0 ? null : string.Join("; ", _errors.Values); } }
    /// <summary>Gets an error for one source without affecting another presenter's status.</summary>
    public string? GetError(ulong source) { lock (_gate) return _errors.GetValueOrDefault(source); }
    /// <summary>Gets whether the current presentation uses the D3D9Ex bridge.</summary>
    public bool UsesGpu { get { lock (_gate) return _recording is not null ? _recordingGpu : _idleSources.Values.Any(source => source.Gpu); } }
    private void Changed(ulong source = 0)
    {
        foreach (var observer in PresentationChanged?.GetInvocationList() ?? [])
            try { ((EventHandler)observer)(this, new PreviewSourceChangedEventArgs(source)); }
            catch (Exception error) { _log.Write("[NativePreview] Observer: " + error.Message); }
    }
    /// <summary>Attaches only a presentation consumer; the caller never owns source/session shutdown.</summary>
    public NativePreview? Attach(ulong source)
    {
        lock (_gate)
        {
            if (_disposed) return null;
            if (_recording is not null) return _recording.AttachPreview(source, _recordingGpu);
            return _idleSources.TryGetValue(source, out var idle) ? idle.Session.AttachPreview(source, idle.Gpu) : null;
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
    public async Task UpdateIdleAsync(MonitorDescriptor? screen, RecordingCameraOptions? camera, RecordingExecutionMode mode)
    {
        long revision = Interlocked.Increment(ref _revision);
        await _control.WaitAsync().ConfigureAwait(false);
        try
        {
            if (_disposed || _suspended || revision != Volatile.Read(ref _revision)) return;
            await UpdateSourceAsync(1, screen is null ? null : new(screen, null, mode), mode).ConfigureAwait(false);
            if (_disposed || _suspended || revision != Volatile.Read(ref _revision)) return;
            await UpdateSourceAsync(2, camera is null ? null : new(null, camera, mode), mode).ConfigureAwait(false);
        }
        finally { _control.Release(); }
    }
    private async Task UpdateSourceAsync(ulong id, IdleSelection? selection, RecordingExecutionMode mode)
    {
        lock (_gate)
        {
            if (_idleSources.TryGetValue(id, out var current) && current.Selection == selection) return;
            if (selection is null && !_idleSources.ContainsKey(id) && !_errors.ContainsKey(id)) return;
        }
        await CloseSourceAsync(id).ConfigureAwait(false);
        lock (_gate) _errors.Remove(id);
        if (selection is null) { Changed(id); return; }
        try
        {
            IdleSource opened;
            try { opened = await OpenSourceAsync(selection, selection.Gpu).ConfigureAwait(false); }
            catch (NativeEngineException error) when (mode == RecordingExecutionMode.Automatic && error.Stage is 3 or 4 or 5 or 9 && error.ResultCode is -4 or -10)
            {
                _log.Write($"[NativePreview] Source {id}: GPU unavailable: {error.Message}");
                opened = await OpenSourceAsync(selection, false).ConfigureAwait(false);
            }
            lock (_gate) _idleSources[id] = opened;
        }
        catch (Exception error)
        {
            lock (_gate) _errors[id] = error.Message;
            _log.Write($"[NativePreview] Source {id}: {error}");
        }
        Changed(id);
    }
    private async Task<IdleSource> OpenSourceAsync(IdleSelection selection, bool gpu)
    {
        NativeEngine? engine = null;
        NativeSession? session = null;
        try
        {
            _log.Write($"[NativePreview] Opening {(selection.Screen is null ? "camera" : "screen")} ({(gpu ? "GPU" : "CPU")}).");
            await Task.Run(() =>
            {
                engine = NativeEngine.Open(applicationDirectory, "NessStudio.IdlePreview");
                var plan = new Dictionary<string, object?> { ["version"] = 1, ["previewOnly"] = true,
                    ["mode"] = gpu ? "RequireGpuResidentHardware" : "FullSoftware" };
                if (selection.Screen is not null) plan["capture"] = new { version = 1, mode = plan["mode"],
                    monitor = ResolveMonitor(engine, selection.Screen), fpsNum = 30, fpsDen = 1,
                    slots = gpu ? 6 : 3, output = new { width = 640, height = 360, oddPolicy = "PadEven" } };
                if (selection.Camera is not null) plan["camera"] = CameraPlan(selection.Camera);
                session = engine.CreateSession(JsonSerializer.SerializeToElement(plan));
            }).ConfigureAwait(false);
            var opening = new IdleSource(selection, engine!, session!, gpu);
            lock (_gate) _openingSource = opening;
            _log.Write($"[NativePreview] Session {session!.SessionId}: preparing.");
            Check(await session!.PrepareAsync().ConfigureAwait(false));
            _log.Write($"[NativePreview] Session {session.SessionId}: starting.");
            Check(await session.StartAsync().ConfigureAwait(false));
            _log.Write($"[NativePreview] Session {session.SessionId}: started.");
            return opening;
        }
        catch
        {
            try { if (session is not null) await session.DisposeAsync().ConfigureAwait(false); }
            catch (Exception error) { _log.Write("[NativePreview] Failed-source cleanup: " + error); }
            finally { engine?.Dispose(); }
            throw;
        }
        finally { lock (_gate) _openingSource = null; }
    }
    private static void Check(NativeCommandCompletion result)
    { if (result.ResultCode < 0) throw new NativeEngineException(result.ResultCode,result.Stage,result.OriginalHResult,result.Message); }
    private async Task CloseSourceAsync(ulong id)
    {
        IdleSource? previous;
        lock (_gate)
        {
            if (!_idleSources.Remove(id, out previous)) return;
            _closingSource = previous;
        }
        Changed(id);
        try { using var final = await previous.Session.CloseAsync().ConfigureAwait(false); CheckSnapshot(final.RootElement); }
        catch (Exception error) { _log.Write($"[NativePreview] Source {id} cleanup: {error}"); }
        finally
        {
            previous.Engine.Dispose();
            lock (_gate) _closingSource = null;
        }
    }
    private async Task CloseIdleAsync()
    {
        await CloseSourceAsync(1).ConfigureAwait(false);
        await CloseSourceAsync(2).ConfigureAwait(false);
    }
    private static void CheckSnapshot(JsonElement root)
    {
        if (root.TryGetProperty("firstError",out var error) && error.ValueKind == JsonValueKind.Object)
            throw new NativeEngineException(error.GetProperty("code").GetInt32(),error.GetProperty("stage").GetUInt32(),error.GetProperty("hresult").GetInt32(),error.GetProperty("message").GetString()!);
    }
    /// <summary>Waits for all idle sources to close before the recorder may prepare the next source owner.</summary>
    public async Task SuspendAsync()
    {
        Interlocked.Increment(ref _revision);
        await _control.WaitAsync().ConfigureAwait(false);
        try { _suspended = true; await CloseIdleAsync().ConfigureAwait(false); }
        finally { _control.Release(); }
    }
    /// <summary>Allows subsequent idle-selection updates after recording ownership has ended.</summary>
    public void ResumeIdle() { lock (_gate) _suspended = false; }
    internal void RecordingPrepared(NativeSession session,bool gpu)
    { lock (_gate) { if (_disposed) return; _recording = session; _recordingGpu = gpu; _errors.Clear(); } Changed(); }
    internal void RecordingEnded(NativeSession session)
    { lock (_gate) { if (!ReferenceEquals(_recording,session)) return; _recording = null; } Changed(); }
    /// <summary>Associates one newly created native recorder with this window's presentation lifecycle.</summary>
    public void Adopt(IRecordingService service)
    { if (service is NativeRecordingService native) native.Workspace = this; }
    /// <inheritdoc />
    public async ValueTask DisposeAsync()
    {
        await _control.WaitAsync().ConfigureAwait(false);
        try { if (_disposed) return; _disposed = true; await CloseIdleAsync().ConfigureAwait(false); lock (_gate) _recording = null; Changed(); PresentationChanged = null; }
        finally { _control.Release(); }
    }
}

/// <summary>Identifies the source whose presenter must rebind; zero means every source.</summary>
public sealed class PreviewSourceChangedEventArgs(ulong sourceId) : EventArgs
{
    /// <summary>Gets the changed native source identity.</summary>
    public ulong SourceId { get; } = sourceId;
}

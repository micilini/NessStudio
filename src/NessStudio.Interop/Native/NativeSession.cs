using System.Runtime.CompilerServices;
using System.Text.Json;

namespace NessStudio.Interop.Native;

/// <summary>Identifies the native session lifecycle independently of presentation state.</summary>
public enum NativeSessionState : uint
{
    /// <summary>Has not been prepared.</summary>
    Created, /// <summary>Is preparing required components.</summary>
    Preparing, /// <summary>Is prepared.</summary>
    Ready, /// <summary>Is awaiting valid first content.</summary>
    Starting, /// <summary>Has admitted required content.</summary>
    Recording, /// <summary>Is acknowledging a common pause cut.</summary>
    Pausing, /// <summary>Has excluded paused content.</summary>
    Paused, /// <summary>Is establishing a resume cut.</summary>
    Resuming, /// <summary>Has stopped accepting new content.</summary>
    Stopping, /// <summary>Is finalizing outputs.</summary>
    Finalizing, /// <summary>Has completed control shutdown; media validity is a separate field.</summary>
    Completed, /// <summary>Has latched a failure; cleanup may remain pending.</summary>
    Failed, /// <summary>Has released session ownership.</summary>
    Disposed
}

/// <summary>Describes a completed native command, including cancellation or failure.</summary>
/// <param name="SessionId">The originating session identity.</param><param name="CommandId">The accepted command identity, or zero for an idempotent response.</param>
/// <param name="Sequence">The native event sequence, or zero for an idempotent response.</param><param name="State">The resulting state.</param>
/// <param name="ResultCode">The actual native completion result; ACCEPTED is never a completion.</param><param name="Stage">The failing stage, if any.</param>
/// <param name="OriginalHResult">The original HRESULT, or zero.</param><param name="Message">The copied diagnostic.</param>
public sealed record NativeCommandCompletion(ulong SessionId, ulong CommandId, ulong Sequence, NativeSessionState State,
    int ResultCode, uint Stage, int OriginalHResult, string Message);

/// <summary>Owns a session with ordered pull events, correlated commands and asynchronous shutdown.</summary>
/// <remarks>Dispose is emergency abandonment. Use CloseAsync or DisposeAsync to await cleanup. Cancelling a command wait never reverses an accepted native operation.</remarks>
public sealed class NativeSession : IDisposable, IAsyncDisposable
{
    private readonly SessionHandle _handle;
    private readonly object _gate = new(), _closeGate = new();
    private readonly Dictionary<ulong, TaskCompletionSource<NativeCommandCompletion>> _pending = [];
    private readonly CancellationTokenSource _lifetime = new();
    private readonly Task _pump;
    private Task<string>? _close;
    private Exception? _pumpError;
    private int _disposed;
    private ulong _sequence;
    private NativeSessionState _state;
    /// <summary>Gets the immutable native session identity.</summary>
    public ulong SessionId { get; }
    /// <summary>Gets the last ordered event sequence consumed by this wrapper.</summary>
    public ulong LastEventSequence { get { lock (_gate) return _sequence; } }
    /// <summary>Gets the state most recently observed by the event pump.</summary>
    public NativeSessionState LastObservedState { get { lock (_gate) return _state; } }

    /// <summary>Attaches a bounded presentation pool; GPU surfaces are D3D9Ex surfaces suitable for D3DImage.</summary>
    /// <param name="sourceId">One for the screen, two for the independent camera.</param>
    /// <param name="gpu">Selects the GPU bridge or a CPU BGRA pool without accelerated presentation dependencies.</param>
    /// <returns>An independently disposable consumer; detaching never stops recording.</returns>
    public NativePreview AttachPreview(ulong sourceId, bool gpu)
    {
        ObjectDisposedException.ThrowIf(Volatile.Read(ref _disposed) != 0, this);
        return NativePreview.Attach(_handle, sourceId, gpu);
    }
    private unsafe ulong AcceptScene(JsonElement scene)
    {
        ObjectDisposedException.ThrowIf(Volatile.Read(ref _disposed) != 0, this);
        byte[] bytes = NativeLibraryLoader.Utf8.GetBytes(scene.GetRawText());
        fixed (byte* data = bytes)
        {
            var error = NativeEngine.NewError();
            int result = NativeMethods.SessionUpdateScene(_handle, new() { data = (nint)data, bytes = (ulong)bytes.Length }, out ulong revision, ref error);
            if (result != 0) NativeEngine.Throw(result, error);
            return revision;
        }
    }
    /// <summary>Copies a scene snapshot and awaits the worker's applied revision without blocking presentation.</summary>
    public async Task<ulong> UpdateSceneAsync(JsonElement scene, CancellationToken cancellationToken = default)
    {
        ulong revision = AcceptScene(scene);
        using var deadline = CancellationTokenSource.CreateLinkedTokenSource(cancellationToken, _lifetime.Token);
        deadline.CancelAfter(TimeSpan.FromSeconds(3));
        while (true)
        {
            using var snapshot = GetSnapshot();
            var root = snapshot.RootElement;
            if (root.GetProperty("metrics").TryGetProperty("sharedScene", out var graph) && graph.GetProperty("appliedRevision").GetUInt64() >= revision) return revision;
            if (root.GetProperty("state").GetUInt32() >= (uint)NativeSessionState.Stopping) throw new InvalidOperationException("The session stopped before applying the scene.");
            await Task.Delay(20, deadline.Token).ConfigureAwait(false);
        }
    }

    internal NativeSession(SessionHandle handle)
    {
        _handle = handle;
        using var initial = GetSnapshot();
        SessionId = initial.RootElement.GetProperty("objectId").GetUInt64();
        _state = (NativeSessionState)initial.RootElement.GetProperty("state").GetUInt32();
        var weak = new WeakReference<NativeSession>(this);
        var token = _lifetime.Token;
        _pump = Task.Run(() => PumpAsync(weak, token)); // Captures no strong session reference.
    }
    /// <summary>Copies a consistent native snapshot; the caller disposes the document.</summary>
    public JsonDocument GetSnapshot()
    {
        ObjectDisposedException.ThrowIf(Volatile.Read(ref _disposed) != 0, this);
        var error = NativeEngine.NewError();
        int result = NativeMethods.SessionSnapshot(_handle, out var blob, ref error);
        using (blob) { if (result != 0) NativeEngine.Throw(result, error); return NativeEngine.Copy(blob); }
    }
    /// <summary>Prepares components and awaits the correlated Ready result.</summary>
    public Task<NativeCommandCompletion> PrepareAsync(CancellationToken cancellationToken = default) => SendAsync(1, cancellationToken);
    /// <summary>Starts and awaits first-content admission or a native failure.</summary>
    public Task<NativeCommandCompletion> StartAsync(CancellationToken cancellationToken = default) => SendAsync(2, cancellationToken);
    /// <summary>Establishes a pause cut and awaits acknowledgement.</summary>
    public Task<NativeCommandCompletion> PauseAsync(CancellationToken cancellationToken = default) => SendAsync(3, cancellationToken);
    /// <summary>Resumes without resetting the media timeline.</summary>
    public Task<NativeCommandCompletion> ResumeAsync(CancellationToken cancellationToken = default) => SendAsync(4, cancellationToken);
    /// <summary>Requests Stop and awaits its actual completion, possibly with cleanup still pending after timeout.</summary>
    public Task<NativeCommandCompletion> StopAsync(CancellationToken cancellationToken = default) => SendAsync(5, cancellationToken);

    private Task<NativeCommandCompletion> SendAsync(uint kind, CancellationToken token)
    {
        try
        {
            Task<NativeCommandCompletion> operation;
            lock (_gate)
            {
                ObjectDisposedException.ThrowIf(_disposed != 0, this); token.ThrowIfCancellationRequested();
                if (_pumpError is not null) throw new InvalidOperationException("Native event pump failed.", _pumpError);
                var error = NativeEngine.NewError();
                int result = NativeMethods.SessionCommand(_handle, kind, out ulong id, ref error);
                if (result < 0) NativeEngine.Throw(result, error);
                if (result == 2 && id == 0)
                {
                    using var snapshot = GetSnapshot();
                    return Task.FromResult(new NativeCommandCompletion(SessionId, 0, 0,
                        (NativeSessionState)snapshot.RootElement.GetProperty("state").GetUInt32(), 2, 0, 0, "Operation already satisfied or session terminal"));
                }
                if (id == 0 || result is not (1 or 2)) throw new InvalidDataException("Native command did not provide an accepted identity.");
                if (!_pending.TryGetValue(id, out var completion))
                {
                    completion = new(TaskCreationOptions.RunContinuationsAsynchronously); _pending.Add(id, completion);
                    // A caller retaining only its accepted command task still owns the
                    // session until completion. An entirely abandoned cycle is collectible.
                    _ = completion.Task.ContinueWith(static (_, owner) => GC.KeepAlive(owner), this,
                        CancellationToken.None, TaskContinuationOptions.ExecuteSynchronously, TaskScheduler.Default);
                }
                operation = completion.Task;
            }
            return token.CanBeCanceled ? operation.WaitAsync(token) : operation;
        }
        catch (Exception error) { return Task.FromException<NativeCommandCompletion>(error); }
    }
    private static async Task PumpAsync(WeakReference<NativeSession> weak, CancellationToken token)
    {
        try
        {
            while (!token.IsCancellationRequested && PollOnce(weak)) await Task.Delay(10, token).ConfigureAwait(false);
        }
        catch (OperationCanceledException) when (token.IsCancellationRequested) { }
    }
    [MethodImpl(MethodImplOptions.NoInlining)]
    private static unsafe bool PollOnce(WeakReference<NativeSession> weak)
    {
        if (!weak.TryGetTarget(out var owner)) return false;
        try
        {
            nss_event_v1 value = new() { size = (uint)sizeof(nss_event_v1), version = 1 };
            var error = NativeEngine.NewError();
            int result = NativeMethods.SessionPoll(owner._handle, 10, ref value, ref error);
            if (result is 3 or -8) return Volatile.Read(ref owner._disposed) == 0;
            if (result != 0) NativeEngine.Throw(result, error);
            byte* bytes = value.message_utf8;
            int length = new ReadOnlySpan<byte>(bytes, 256).IndexOf((byte)0);
            string message = NativeLibraryLoader.Utf8.GetString(new ReadOnlySpan<byte>(bytes, length < 0 ? 256 : length));
            lock (owner._gate)
            {
                if (owner._disposed != 0) return false;
                if (value.object_id != owner.SessionId || value.sequence != owner._sequence + 1) throw new InvalidDataException("Out-of-order or foreign native session event.");
                owner._sequence = value.sequence; owner._state = (NativeSessionState)value.state;
                if (value.kind == 1 && owner._pending.Remove(value.command_id, out var completion))
                    completion.TrySetResult(new(owner.SessionId, value.command_id, value.sequence, owner._state, value.status, value.stage, value.hresult, message));
            }
            return true;
        }
        catch (ObjectDisposedException) when (Volatile.Read(ref owner._disposed) != 0) { return false; }
        catch (Exception error)
        {
            lock (owner._gate) { owner._pumpError = error; foreach (var completion in owner._pending.Values) completion.TrySetException(error); owner._pending.Clear(); }
            return false;
        }
    }
    /// <summary>Stops and awaits resource cleanup; cancellation affects only this caller's wait.</summary>
    /// <returns>A caller-owned final snapshot retaining the original error and media validity.</returns>
    public async Task<JsonDocument> CloseAsync(CancellationToken cancellationToken = default)
    {
        Task<string> closing;
        lock (_closeGate) closing = _close ??= CloseCoreAsync();
        string json = await closing.WaitAsync(cancellationToken).ConfigureAwait(false);
        return JsonDocument.Parse(json);
    }
    private async Task<string> CloseCoreAsync()
    {
        try
        {
            await StopAsync().ConfigureAwait(false);
            for (;;)
            {
                using var snapshot = GetSnapshot();
                if (!snapshot.RootElement.GetProperty("cleanupPending").GetBoolean()) return snapshot.RootElement.GetRawText();
                await Task.Delay(10).ConfigureAwait(false);
            }
        }
        finally
        {
            Dispose();
            await _pump.ConfigureAwait(false);
            _lifetime.Dispose();
        }
    }
    /// <summary>Transfers native ownership to emergency cleanup without waiting on the calling thread.</summary>
    public void Dispose()
    {
        lock (_gate)
        {
            if (Interlocked.Exchange(ref _disposed, 1) != 0) return;
            _lifetime.Cancel();
            try { NativeMethods.SessionInterrupt(_handle); } catch (ObjectDisposedException) { }
            _handle.Dispose();
            foreach (var completion in _pending.Values) completion.TrySetException(new ObjectDisposedException(nameof(NativeSession)));
            _pending.Clear();
        }
    }
    /// <summary>Awaits Stop and cleanup before releasing session ownership.</summary>
    public async ValueTask DisposeAsync() { using var snapshot = await CloseAsync().ConfigureAwait(false); }
}

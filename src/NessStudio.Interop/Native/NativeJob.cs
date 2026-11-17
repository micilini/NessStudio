using System.Text.Json;

namespace NessStudio.Interop.Native;

/// <summary>Owns a supervised native export job. Media and child processes remain native.</summary>
public sealed class NativeJob : IDisposable, IAsyncDisposable
{
    private readonly JobHandle _handle;
    private readonly object _gate = new();
    private Task<JsonDocument>? _run;
    private int _disposed;

    internal NativeJob(JobHandle handle) => _handle = handle;

    /// <summary>Copies the native state, progress and bounded diagnostics.</summary>
    /// <returns>A caller-owned JSON snapshot.</returns>
    public JsonDocument GetSnapshot()
    {
        ObjectDisposedException.ThrowIf(Volatile.Read(ref _disposed) != 0, this);
        var error = NativeEngine.NewError();
        int result = NativeMethods.JobSnapshot(_handle, out var blob, ref error);
        using (blob) { if (result != 0) NativeEngine.Throw(result, error); return NativeEngine.Copy(blob); }
    }

    /// <summary>Starts once and awaits a terminal native result on a background worker.</summary>
    /// <param name="cancellationToken">Requests native cancellation; the returned snapshot acknowledges its completion.</param>
    /// <returns>A caller-owned terminal snapshot, including native failures.</returns>
    public Task<JsonDocument> RunAsync(CancellationToken cancellationToken = default)
    {
        lock (_gate)
        {
            ObjectDisposedException.ThrowIf(_disposed != 0, this);
            if (_run is not null) throw new InvalidOperationException("This job has already started.");
            cancellationToken.ThrowIfCancellationRequested();
            var error = NativeEngine.NewError();
            int result = NativeMethods.JobStart(_handle, out ulong command, ref error);
            if (result != 1) NativeEngine.Throw(result, error);
            _run = Task.Run(() =>
            {
                using var registration = cancellationToken.Register(Cancel);
                ulong sequence = 0, objectId = 0;
                for (;;)
                {
                    nss_event_v1 value = new() { size = 320, version = 1 };
                    var diagnostic = NativeEngine.NewError();
                    int status = NativeMethods.JobPoll(_handle, 100, ref value, ref diagnostic);
                    if (status == 3) continue;
                    if (status != 0) NativeEngine.Throw(status, diagnostic);
                    if (value.sequence != ++sequence || (objectId != 0 && objectId != value.object_id))
                        throw new InvalidDataException("Native job event identity/sequence mismatch.");
                    objectId = value.object_id;
                    if (value.state is 104 or 105 or 106)
                    {
                        if (value.command_id != command && value.state != 104)
                            throw new InvalidDataException("Native job completion command mismatch.");
                        return GetSnapshot();
                    }
                }
            }, CancellationToken.None);
            return _run;
        }
    }

    /// <summary>Requests cancellation without blocking for process termination.</summary>
    public void Cancel()
    {
        lock (_gate)
        {
            if (_disposed != 0) return;
            var error = NativeEngine.NewError();
            int status = NativeMethods.JobCancel(_handle, out _, ref error);
            if (status is not (1 or 2 or -3)) NativeEngine.Throw(status, error);
        }
    }

    /// <summary>Transfers emergency cancellation and process cleanup to the native reaper.</summary>
    public void Dispose()
    {
        lock (_gate)
        {
            if (Interlocked.Exchange(ref _disposed, 1) != 0) return;
            _handle.Dispose();
        }
    }

    /// <summary>Cancels, waits for an active operation, then releases ownership.</summary>
    public async ValueTask DisposeAsync()
    {
        Task<JsonDocument>? operation;
        lock (_gate) operation = _run;
        try { Cancel(); if (operation is not null) await operation.ConfigureAwait(false); }
        finally { Dispose(); }
    }
}

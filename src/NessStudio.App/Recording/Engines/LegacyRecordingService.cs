using System.Diagnostics;
using System.IO;
using System.Text.Json;
using NessStudio.Models;
using NessStudio.ViewModel.Helpers;

namespace NessStudio.Recording.Engines;

/// <summary>Serializes legacy operations and exposes their real completion without introducing another media clock.</summary>
internal sealed class LegacyRecordingService(IRecorderEngine backend, RecordingCapabilities capabilities,
    string outputDirectory) : IRecordingService
{
    // Kept as a field so diagnostic probes can explicitly inspect the real backend.
    private readonly IRecorderEngine _backend = backend;
    private readonly SemaphoreSlim _control = new(1, 1);
    private int _state = (int)RecordingState.Created;
    private readonly object _disposeSync = new();
    private Task? _disposeTask;
    private int _disposeRequested;
    private bool _started;
    public event EventHandler<RecordingStateChangedEventArgs>? StateChanged;
    public RecordingState State => (RecordingState)Volatile.Read(ref _state);
    public RecordingCapabilities Capabilities { get; } = capabilities;
    private RecordingStatistics _statistics = new(null, null, null);
    public RecordingStatistics Statistics
    {
        get => _statistics.ActiveDuration is not null ? _statistics : _statistics with { ActiveDuration = (_backend as RecordAssist)?.ActiveDuration };
        private set => _statistics = value;
    }
    public RecordingResult? Result { get; private set; }

    private void Publish(RecordingState state, Exception? error = null)
    {
        Volatile.Write(ref _state, (int)state);
        var notification = new RecordingStateChangedEventArgs(state, error);
        foreach (var handler in StateChanged?.GetInvocationList() ?? [])
        {
            try { ((EventHandler<RecordingStateChangedEventArgs>)handler)(this, notification); }
            catch (Exception ex) { DebugLog.Write("[RecordingService] observer failed: " + UserError.Message(ex)); }
        }
    }

    private async Task RunAsync(RecordingState transient, RecordingState completed, Func<Task> operation,
        params RecordingState[] allowed)
    {
        ObjectDisposedException.ThrowIf(Volatile.Read(ref _disposeRequested) != 0, this);
        await _control.WaitAsync().ConfigureAwait(false);
        try
        {
            ObjectDisposedException.ThrowIf(State == RecordingState.Disposed || Volatile.Read(ref _disposeRequested) != 0, this);
            if (State == completed) return;
            if (!allowed.Contains(State)) throw new InvalidOperationException($"Cannot enter {transient} from {State}.");
            try
            {
                Publish(transient);
                await operation().ConfigureAwait(false);
                Publish(completed);
            }
            catch (Exception ex) { Publish(RecordingState.Failed, ex); throw; }
        }
        finally { _control.Release(); }
    }

    public Task PrepareAsync() => RunAsync(RecordingState.Preparing, RecordingState.Ready,
        _backend.PrepareAsync, RecordingState.Created);
    public Task StartAsync() => RunAsync(RecordingState.Starting, RecordingState.Recording,
        async () => { await _backend.StartAsync().ConfigureAwait(false); _started = true; }, RecordingState.Ready);
    public Task PauseAsync() => RunAsync(RecordingState.Pausing, RecordingState.Paused,
        _backend.PauseAsync, RecordingState.Recording);
    public Task ResumeAsync() => RunAsync(RecordingState.Resuming, RecordingState.Recording,
        _backend.ResumeAsync, RecordingState.Paused);
    public Task StopAsync() => RunAsync(RecordingState.Stopping, RecordingState.Finalizing,
        _backend.StopAsync, RecordingState.Ready, RecordingState.Recording, RecordingState.Paused);

    public async Task<string> StopAndFinalizeAsync(IProgress<RecordingSaveProgress>? progress = null)
    {
        ObjectDisposedException.ThrowIf(Volatile.Read(ref _disposeRequested) != 0, this);
        await _control.WaitAsync().ConfigureAwait(false);
        try
        {
            ObjectDisposedException.ThrowIf(State == RecordingState.Disposed || Volatile.Read(ref _disposeRequested) != 0, this);
            if (State == RecordingState.Completed) return Result!.PrimaryAssetPath;
            if (!_started) throw new InvalidOperationException("A session that did not start cannot be reported as finalized media.");
            if (State is not (RecordingState.Recording or RecordingState.Paused or RecordingState.Finalizing))
                throw new InvalidOperationException($"Cannot finalize from {State}.");
            var clock = Stopwatch.StartNew();
            try
            {
                Publish(RecordingState.Finalizing);
                string primaryAsset = await _backend.StopAndFinalizeAsync(progress).ConfigureAwait(false);
                if (string.IsNullOrWhiteSpace(primaryAsset) || !File.Exists(primaryAsset))
                    throw new InvalidDataException("The legacy backend did not finalize a primary media asset.");
                string manifestPath = Path.Combine(outputDirectory, "manifest.json");
                var manifest = JsonSerializer.Deserialize<SessionManifest>(await File.ReadAllTextAsync(manifestPath).ConfigureAwait(false))
                    ?? throw new InvalidDataException("The finalized session manifest is empty.");
                Statistics = new(manifest.DurationMs is { } duration ? TimeSpan.FromMilliseconds(duration) : null,
                    manifest.Screen?.FrameCount, clock.Elapsed);
                Result = new(outputDirectory, primaryAsset, manifestPath, Capabilities.Backend, Statistics);
                Publish(RecordingState.Completed);
                return primaryAsset;
            }
            catch (Exception ex) { Publish(RecordingState.Failed, ex); throw; }
        }
        finally { _control.Release(); }
    }

    // Existing legacy operations cannot be cancelled after admission. Cleanup
    // waits for their completion; it never frees a backend still using resources.
    private async Task ReleaseAsync()
    {
        await _control.WaitAsync().ConfigureAwait(false);
        try
        {
            if (State == RecordingState.Disposed) return;
            await Task.Run(_backend.Dispose).ConfigureAwait(false);
            Publish(RecordingState.Disposed);
            StateChanged = null;
        }
        finally { _control.Release(); }
    }
    public ValueTask DisposeAsync()
    {
        lock (_disposeSync)
        {
            Interlocked.Exchange(ref _disposeRequested, 1);
            return new ValueTask(_disposeTask ??= ReleaseAsync());
        }
    }
    public void Dispose()
    {
        Task cleanup;
        lock (_disposeSync)
        {
            Interlocked.Exchange(ref _disposeRequested, 1);
            cleanup = _disposeTask ??= ReleaseAsync();
        }
        if (cleanup.IsCompletedSuccessfully) return;
        // Synchronous fallback retains the service until cleanup finishes. UI
        // owners use DisposeAsync; the harness normally disposes after completion.
        _ = ObserveCleanupAsync(cleanup);
    }
    private static async Task ObserveCleanupAsync(Task cleanup)
    {
        try { await cleanup.ConfigureAwait(false); }
        catch (Exception ex) { DebugLog.Write("[RecordingService] cleanup failed: " + UserError.Details(ex)); }
    }
}

using System.Runtime.CompilerServices;
using System.Text.Json;
using NessStudio.Interop.Native;

/// <summary>Exercises the production wrapper against the explicitly isolated synthetic-backend DLL.</summary>
internal static class SessionCases
{
    private static void Check(bool value, string message) { if (!value) throw new InvalidDataException(message); }
    private static NativeSession Create(NativeEngine engine, object? options = null)
    {
        using var request = JsonDocument.Parse(JsonSerializer.Serialize(new { version = 1, testFixture = options ?? new { } }));
        return engine.CreateSession(request.RootElement);
    }
    private static JsonElement Snapshot(NativeSession session) { using var value = session.GetSnapshot(); return value.RootElement.Clone(); }
    private static async Task ReadyAsync(NativeSession session) { Check((await session.PrepareAsync()).ResultCode == 0, "Prepare completion failed"); }
    private static async Task StartAsync(NativeSession session) { await ReadyAsync(session); Check((await session.StartAsync()).State == NativeSessionState.Recording, "Start did not await valid first content"); }
    [MethodImpl(MethodImplOptions.NoInlining)]
    private static void Abandon(string root)
    {
        using var engine = NativeEngine.Open(root);
        var session = Create(engine); GC.KeepAlive(session);
    }
    [MethodImpl(MethodImplOptions.NoInlining)]
    private static (Task<NativeCommandCompletion>, WeakReference<NativeSession>) StartUnrootedCommand(NativeEngine engine)
    {
        var session = Create(engine, new { prepareDelayMs = 200 });
        return (session.PrepareAsync(), new WeakReference<NativeSession>(session));
    }
    public static async Task<int> RunAsync(string root)
    {
        try
        {
            using var deadline = new CancellationTokenSource(TimeSpan.FromSeconds(60));
            var cases = new List<string>();
            var states = new List<NativeSessionState>();
            NativeBuildInformation build;
            using (var engine = NativeEngine.Open(root, "sessão 🎬"))
            {
                build = engine.Build;
                using var capabilities = engine.GetCapabilities();
                Check(capabilities.RootElement.GetProperty("syntheticTestBackend").GetBoolean(), "Session tests must never run against a production fake backend");
                await using (var session = Create(engine))
                {
                    states.Add((await session.PrepareAsync()).State); states.Add((await session.StartAsync()).State);
                    ulong sequence = session.LastEventSequence;
                    Check((await session.StartAsync()).ResultCode == 2 && session.LastEventSequence == sequence, "Idempotent Start created a second operation");
                    states.Add((await session.PauseAsync()).State); states.Add((await session.ResumeAsync()).State); states.Add((await session.StopAsync()).State);
                    Check(states.SequenceEqual(new[] { NativeSessionState.Ready, NativeSessionState.Recording, NativeSessionState.Paused, NativeSessionState.Recording, NativeSessionState.Completed }), "Wrapper state sequence diverged");
                    var final = Snapshot(session);
                    Check(!final.GetProperty("mediaComplete").GetBoolean() && !final.GetProperty("cleanupPending").GetBoolean(), "Synthetic session announced real completed media");
                    Check(final.GetProperty("metrics").GetProperty("poolAcquired").GetUInt64() == final.GetProperty("metrics").GetProperty("poolReleased").GetUInt64(), "Frame leases were not returned");
                    cases.Add("correlated-states-idempotent-start-immutable-snapshot");
                }
                await using (var session = Create(engine, new { prepareDelayMs = 150 }))
                {
                    var preparing = session.PrepareAsync();
                    Check(!preparing.IsCompleted, "ACCEPTED was returned as operation completion");
                    var stopped = session.StopAsync();
                    Check((await preparing).ResultCode == -8 && (await stopped).ResultCode == -8, "Stop did not cancel Prepare without claiming media");
                    cases.Add("stop-during-prepare-correlated-cancellation");
                }
                await using (var session = Create(engine, new { prepareDelayMs = 120 }))
                {
                    using var cancelled = new CancellationTokenSource(); cancelled.Cancel();
                    bool rejected = false;
                    try { await session.PrepareAsync(cancelled.Token); } catch (OperationCanceledException) { rejected = true; }
                    Check(rejected && Snapshot(session).GetProperty("state").GetUInt32() == 0, "Cancelled pre-submission wait mutated native state");
                    using var wait = new CancellationTokenSource(20);
                    rejected = false;
                    try { await session.PrepareAsync(wait.Token); } catch (OperationCanceledException) { rejected = true; }
                    Check(rejected, "Accepted-command wait was not cancelled");
                    for (int i = 0; Snapshot(session).GetProperty("state").GetUInt32() != 2; ++i) { Check(i < 100, "Cancellation reversed or stranded the native Prepare"); await Task.Delay(10, deadline.Token); }
                    cases.Add("cancel-before-accept-and-cancel-wait-after-accept");
                }
                await using (var session = Create(engine))
                {
                    await StartAsync(session); await session.PauseAsync();
                    long active = Snapshot(session).GetProperty("activeTimeHns").GetInt64();
                    await Task.Delay(40, deadline.Token);
                    Check(Snapshot(session).GetProperty("activeTimeHns").GetInt64() == active, "Paused active clock advanced");
                    Check((await session.StopAsync()).State == NativeSessionState.Completed, "Stop during pause failed");
                    cases.Add("stop-in-pause-active-clock-frozen");
                }
                await using (var session = Create(engine, new { emptyStart = true }))
                {
                    await ReadyAsync(session); var result = await session.StartAsync();
                    Check(result.State == NativeSessionState.Failed && result.ResultCode == -3, "Empty Start fabricated Recording");
                    cases.Add("first-content-required-before-recording");
                }
                await using (var session = Create(engine, new { faultStage = 3, faultAt = "tick", faultDelayMs = 100, secondaryOutputFailure = true }))
                {
                    await StartAsync(session);
                    while (!Snapshot(session).GetProperty("metrics").GetProperty("faultEntered").GetBoolean()) await Task.Delay(1, deadline.Token);
                    var result = await session.StopAsync();
                    using var final = await session.CloseAsync(deadline.Token);
                    Check(result.ResultCode == -6 && result.Stage == 3 && final.RootElement.GetProperty("firstError").GetProperty("stage").GetInt32() == 3 && final.RootElement.GetProperty("secondaryErrors").GetArrayLength() == 1, "Concurrent Stop replaced the causal source failure");
                    cases.Add("source-error-concurrent-stop-secondary-cleanup");
                }
                await using (var session = Create(engine, new { drainDelayMs = 150 }))
                {
                    await StartAsync(session);
                    using var wait = new CancellationTokenSource(20); bool cancelled = false;
                    try { using var ignored = await session.CloseAsync(wait.Token); } catch (OperationCanceledException) { cancelled = true; }
                    Check(cancelled, "Close caller cancellation did not cancel its wait");
                    using var final = await session.CloseAsync(deadline.Token);
                    Check(!final.RootElement.GetProperty("cleanupPending").GetBoolean(), "Close cancellation freed an active native cleanup");
                    cases.Add("close-cancellation-keeps-cleanup-alive-and-awaitable");
                }
                {
                    var (pending, weak) = StartUnrootedCommand(engine);
                    GC.Collect(); GC.WaitForPendingFinalizers();
                    Check(weak.TryGetTarget(out _), "Retaining a command Task did not retain its native session owner");
                    Check((await pending.WaitAsync(deadline.Token)).State == NativeSessionState.Ready, "Unrooted accepted task did not complete");
                    // Release engine budget through the recovered owner, then await actual teardown.
                    if (weak.TryGetTarget(out var owner)) { using var final = await owner.CloseAsync(deadline.Token); }
                    cases.Add("accepted-task-retains-owner-until-completion");
                }
                {
                    var session = Create(engine, new { prepareDelayMs = 200 });
                    var preparing = session.PrepareAsync(); session.Dispose(); session.Dispose();
                    bool disposed = false; try { await preparing; } catch (ObjectDisposedException) { disposed = true; }
                    Check(disposed, "Emergency Dispose left an unresolved command waiter");
                    cases.Add("emergency-double-dispose-with-pending-events");
                }
            }
            // Weak event-pump ownership must permit GC without requiring the user to Dispose.
            for (int batch = 0; batch < 4; ++batch)
            {
                for (int i = 0; i < 16; ++i) Abandon(root);
                bool quiet = false;
                for (int attempt = 0; attempt < 80 && !quiet; ++attempt)
                {
                    GC.Collect(); GC.WaitForPendingFinalizers(); GC.Collect();
                    quiet = NativeEngine.WaitForQuiescence(20);
                    if (!quiet) await Task.Delay(17, deadline.Token);
                }
                Check(quiet, "Abandoned wrapper/event pump leaked a native context or session");
            }
            cases.Add("gc-64-abandoned-sessions-real-native-quiescence");
            using (var engine = NativeEngine.Open(root))
            {
                var session = Create(engine); engine.Dispose();
                await StartAsync(session); using var final = await session.CloseAsync(deadline.Token);
                Check(final.RootElement.GetProperty("state").GetUInt32() == 10, "Child session depended on a released engine handle");
            }
            cases.Add("session-outlives-engine-client-handle");
            Check(NativeEngine.WaitForQuiescence(1000), "Final native session cleanup did not quiesce");
            Console.WriteLine(JsonSerializer.Serialize(new { status = "PASS", scope = "session", synthetic = true, build, states, cases, native_quiescence = true }, new JsonSerializerOptions { WriteIndented = true }));
            return 0;
        }
        catch (Exception error) { Console.WriteLine(JsonSerializer.Serialize(new { status = "FAIL", error = error.ToString() })); return 1; }
    }
}

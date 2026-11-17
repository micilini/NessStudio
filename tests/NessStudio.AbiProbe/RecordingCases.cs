using System.Diagnostics;
using System.Text.Json;
using NessStudio.Interop.Native;

/// <summary>Exercises physical video through the existing LibraryImport session consumer.</summary>
internal static class RecordingCases
{
    internal static async Task<int> RunAsync(string root, string configurationPath)
    {
        try
        {
            using var configuration = JsonDocument.Parse(await File.ReadAllBytesAsync(configurationPath));
            using var engine = NativeEngine.Open(root, "gravação ação 🎬");
            var states = new List<NativeSessionState>();
            JsonElement snapshot;
            await using (var session = engine.CreateSession(configuration.RootElement))
            {
                async Task Completed(Task<NativeCommandCompletion> pending)
                {
                    var result = await pending.WaitAsync(TimeSpan.FromSeconds(8));
                    if (result.ResultCode != 0 || result.SessionId != session.SessionId)
                        throw new InvalidDataException("Physical command did not complete on its own session: " + result.Message);
                    states.Add(result.State);
                }
                await Completed(session.PrepareAsync());
                GC.Collect(); GC.WaitForPendingFinalizers();
                await Completed(session.StartAsync());
                await Task.Delay(1000);
                await Completed(session.PauseAsync());
                using var paused = session.GetSnapshot();
                long cut = paused.RootElement.GetProperty("activeTimeHns").GetInt64();
                await Task.Delay(300);
                using var held = session.GetSnapshot();
                if (held.RootElement.GetProperty("activeTimeHns").GetInt64() != cut)
                    throw new InvalidDataException("Paused useful time advanced.");
                await Completed(session.ResumeAsync());
                long target = configuration.RootElement.GetProperty("durationMs").GetInt64() * 10000;
                var deadline = Stopwatch.StartNew();
                for (;;)
                {
                    using var current = session.GetSnapshot();
                    if (current.RootElement.GetProperty("activeTimeHns").GetInt64() >= target) break;
                    if (deadline.Elapsed > TimeSpan.FromSeconds(15)) throw new TimeoutException("Managed physical recording timed out.");
                    await Task.Delay(20);
                }
                await Completed(session.StopAsync());
                using var final = session.GetSnapshot();
                snapshot = final.RootElement.Clone();
                if (!snapshot.GetProperty("mediaComplete").GetBoolean() || snapshot.GetProperty("cleanupPending").GetBoolean() ||
                    snapshot.GetProperty("metrics").GetProperty("inflightLeases").GetUInt64() != 0)
                    throw new InvalidDataException("Physical session did not finalize and release its leases.");
            }
            if (!states.SequenceEqual([NativeSessionState.Ready, NativeSessionState.Recording, NativeSessionState.Paused, NativeSessionState.Recording, NativeSessionState.Completed]))
                throw new InvalidDataException("Managed physical state sequence mismatch.");
            engine.Dispose();
            if (!NativeEngine.WaitForQuiescence(1000)) throw new InvalidDataException("Physical managed recording did not quiesce.");
            Console.WriteLine(JsonSerializer.Serialize(new { status = "PASS", scope = "physical-recording", build = engine.Build, states, snapshot, nativeQuiescence = true }));
            return 0;
        }
        catch (Exception error)
        {
            Console.WriteLine(JsonSerializer.Serialize(new { status = "FAIL", error = error.GetType().Name, message = error.Message }));
            return 2;
        }
    }
}

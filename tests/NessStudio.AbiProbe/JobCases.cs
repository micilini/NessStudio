using System.Text.Json;
using NessStudio.Interop.Native;

internal static class JobCases
{
    internal static async Task<int> RunAsync(string root, string path, bool cancel)
    {
        try
        {
            using var description = JsonDocument.Parse(await File.ReadAllBytesAsync(path));
            using var engine = NativeEngine.Open(root, "exportação \uD83C\uDFAC");
            using var microphones = engine.GetAudioSources(false);
            using var loopbacks = engine.GetAudioSources(true);
            if (microphones.RootElement.GetProperty("acceleratedDeviceCreated").GetBoolean() ||
                loopbacks.RootElement.GetProperty("acceleratedDeviceCreated").GetBoolean())
                throw new InvalidDataException("Audio enumeration initialized an accelerated device.");
            await using var job = engine.CreateJob(description.RootElement);
            engine.Dispose(); // The job must retain its own budget/context.
            using var cancellation = new CancellationTokenSource();
            var operation = job.RunAsync(cancellation.Token);
            if (cancel) cancellation.Cancel();
            using var result = await operation;
            bool complete = result.RootElement.GetProperty("mediaComplete").GetBoolean();
            uint state = result.RootElement.GetProperty("state").GetUInt32();
            if (cancel ? complete || state != 104 : !complete || state != 105)
                throw new InvalidDataException("Native job completion/cancellation differs from its acknowledgement.");
            await job.DisposeAsync();
            if (!NativeEngine.WaitForQuiescence(1000)) throw new InvalidDataException("Job/process ownership did not quiesce.");
            Console.WriteLine(JsonSerializer.Serialize(new { status = "PASS", scope = "native-av-job", cancel, engineReleasedBeforeJobStarted = true, terminal = result.RootElement.Clone() }));
            return 0;
        }
        catch (Exception error)
        {
            Console.WriteLine(JsonSerializer.Serialize(new { status = "FAIL", scope = "native-av-job", message = error.Message }));
            return 1;
        }
    }
}

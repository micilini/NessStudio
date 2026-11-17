using System.Text.Json;
using NessStudio.Interop.Native;

internal static class CaptureCases
{
    internal static async Task<int> RunAsync(string root, string configurationPath)
    {
        try
        {
            using var configuration = JsonDocument.Parse(await File.ReadAllBytesAsync(configurationPath));
            using var engine = NativeEngine.Open(root, "captura \u00e7\u00e3o \ud83c\udfac");
            if (engine.Build.AbiMinor < 2) throw new InvalidDataException("Capture requires ABI 1.2.");
            var cases = new List<string>();
            using (var sources = engine.GetScreenSources())
            {
                if (sources.RootElement.GetProperty("devices").GetArrayLength() == 0 || sources.RootElement.GetProperty("acceleratedDeviceCreated").GetBoolean())
                    throw new InvalidDataException("Enumeration omitted monitors or created a graphics device.");
                cases.Add("real-physical-screen-enumeration-without-device");
            }
            foreach (string text in new[] { "{\"version\":2}", "{\"version\":1,\"testFixture\":{}}", "{\"version\":1,\"fpsNum\":30.5}", "{\"version\":1,\"maxBytes\":1}" })
            {
                using var invalid = JsonDocument.Parse(text);
                bool rejected = false;
                try { using var result = engine.ProbeCapture(invalid.RootElement); }
                catch (NativeEngineException error) { rejected = error.ResultCode < 0 && error.Stage == 2; }
                if (!rejected) throw new InvalidDataException("A malformed capture request was accepted or lost its plan-stage cause.");
                cases.Add("reject-capture-control-" + cases.Count);
            }
            GC.Collect(); GC.WaitForPendingFinalizers();
            using var report = await Task.Run(() => engine.ProbeCapture(configuration.RootElement));
            var actual = report.RootElement;
            if (actual.GetProperty("status").GetString() != "PASS" || actual.GetProperty("missingSlots").GetUInt64() != 0 || actual.GetProperty("encoderUsed").GetBoolean() ||
                actual.GetProperty("livePipelines").GetUInt64() != 0 || actual.GetProperty("liveSurfaces").GetUInt64() != 0 || actual.GetProperty("liveCaptureBytes").GetUInt64() != 0)
                throw new InvalidDataException("Native capture report has missing content or live resources.");
            cases.Add("real-native-capture-processor-on-owned-worker");
            engine.Dispose();
            if (!NativeEngine.WaitForQuiescence(1000)) throw new InvalidDataException("Capture objects did not quiesce after managed disposal.");
            cases.Add("capture-safehandle-and-native-quiescence");
            Console.WriteLine(JsonSerializer.Serialize(new { status = "PASS", scope = "capture", build = engine.Build, cases, report = actual.Clone(), native_quiescence = true }));
            return 0;
        }
        catch (Exception error)
        {
            Console.WriteLine(JsonSerializer.Serialize(new { status = "FAIL", error = error.GetType().Name, message = error.Message }));
            return 2;
        }
    }
}

using System.Reflection;
using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;
using System.Text;
using System.Text.Json;
using NessStudio.Interop.Native;

internal static class Program
{
    private static void Check(bool condition, string message) { if (!condition) throw new InvalidDataException(message); }
    private static unsafe void InvalidConfiguration(byte[] input, int expected, uint size = 24, uint version = 1, ulong? length = null, bool invalidError = false)
    {
        fixed (byte* data = input)
        {
            var config = new nss_json_v1 { size = size, version = version, json = new() { data = (nint)data, bytes = length ?? (ulong)input.Length } };
            var error = NativeEngine.NewError(); if (invalidError) error.size = 8;
            int result = NativeMethods.EngineCreate(ref config, out var engine, ref error);
            using (engine) { Check(result == expected && engine.IsInvalid, "Invalid configuration produced success/ownership: " + result); }
            if (!invalidError) Check(error.code == expected, "Structured error did not preserve the result code.");
        }
    }
    [MethodImpl(MethodImplOptions.NoInlining)]
    private static void Abandon(string directory) { var engine = NativeEngine.Open(directory, "GC-owned"); using var document = engine.GetCapabilities(); }
    private static async Task<int> Main(string[] args)
    {
        if(args.Contains("--scene-contracts"))return SceneCases.Run();
        if (args.Contains("--job-config"))
        {
            int rootIndex=Array.IndexOf(args,"--root"),index=Array.IndexOf(args,"--job-config");
            return await JobCases.RunAsync(rootIndex>=0?args[rootIndex+1]:AppContext.BaseDirectory,args[index+1],args.Contains("--cancel-job"));
        }
        if (args.Contains("--sessions"))
        {
            int index = Array.IndexOf(args, "--root");
            return await SessionCases.RunAsync(index >= 0 && index + 1 < args.Length ? args[index + 1] : AppContext.BaseDirectory);
        }
        if (args.Contains("--capture-config"))
        {
            int rootIndex = Array.IndexOf(args, "--root");
            int configIndex = Array.IndexOf(args, "--capture-config");
            return await CaptureCases.RunAsync(rootIndex >= 0 && rootIndex + 1 < args.Length ? args[rootIndex + 1] : AppContext.BaseDirectory, args[configIndex + 1]);
        }
        if (args.Contains("--record-config"))
        {
            int rootIndex = Array.IndexOf(args, "--root"), configIndex = Array.IndexOf(args, "--record-config");
            return await RecordingCases.RunAsync(rootIndex >= 0 && rootIndex + 1 < args.Length ? args[rootIndex + 1] : AppContext.BaseDirectory, args[configIndex + 1]);
        }
        try
        {
            string root = AppContext.BaseDirectory;
            string? software = null;
            for (int i = 0; i < args.Length; i++)
            {
                if (args[i] == "--root") root = args[++i];
                else if (args[i] == "--software") software = args[++i];
            }
            var engine = NativeEngine.Open(root, "a\u00e7\u00e3o \ud83c\udfac \u6771\u4eac");
            var build = engine.Build;
            var cases = new List<string>();
            using (var capabilities = engine.GetCapabilities())
            {
                Check(capabilities.RootElement.GetProperty("clientName").GetString() == "a\u00e7\u00e3o \ud83c\udfac \u6771\u4eac", "UTF-8 control data was not copied correctly.");
                Check(capabilities.RootElement.GetProperty("declaredCapabilities").GetProperty("recording").GetBoolean() &&
                    capabilities.RootElement.GetProperty("recordingScope").GetProperty("audio").GetBoolean() &&
                    capabilities.RootElement.GetProperty("declaredCapabilities").GetProperty("jobs").GetBoolean(), "Physical A/V recording scope was not declared correctly.");
                Check(capabilities.RootElement.GetProperty("build").GetString() == build.Build, "Bootstrap and capability builds disagree.");
                Check(!capabilities.RootElement.GetProperty("encoders").EnumerateArray().Any(encoder =>
                    encoder.GetProperty("id").GetString()!.StartsWith("mf-", StringComparison.Ordinal)),
                    "The native package still advertises Media Foundation encoders.");
                cases.Add("real-context-unicode-capabilities");
                cases.Add("native-encoders-without-media-foundation");
            }
            GC.Collect(); GC.WaitForPendingFinalizers();
            using (var afterCollection = engine.GetCapabilities())
                Check(afterCollection.RootElement.GetProperty("clientName").GetString() == "a\u00e7\u00e3o \ud83c\udfac \u6771\u4eac", "Input bytes were borrowed after return.");
            cases.Add("control-copy-survives-managed-gc");
            foreach (var invalid in new[] { ("{\"version\":2}", -2), ("{\"version\":1,\"extra\":1}", -1),
                ("{\"version\":1,\"maxActiveSessions\":1.9}", -1), ("{\"version\":1,\"clientName\":\"\\u0000\"}", -1),
                ("{\"version\":1,\"diagnostics\":\"Unknown\"}", -1), ("{", -1), ("null", -1) })
            { InvalidConfiguration(Encoding.UTF8.GetBytes(invalid.Item1), invalid.Item2); cases.Add("reject-" + cases.Count); }
            InvalidConfiguration([0xc0, 0xaf], -1); cases.Add("reject-invalid-utf8");
            InvalidConfiguration([0], -1); cases.Add("reject-nul");
            InvalidConfiguration([123], -1, length: 1048577); cases.Add("reject-oversized-before-reading");
            InvalidConfiguration([123], -2, size: 8); cases.Add("reject-envelope-size");
            InvalidConfiguration([123], -2, version: 99); cases.Add("reject-envelope-version");
            InvalidConfiguration([123], -2, invalidError: true); cases.Add("reject-error-envelope");
            var deep = Encoding.ASCII.GetBytes(new string('[', 65) + new string(']', 65));
            InvalidConfiguration(deep, -1); cases.Add("reject-deep-json-before-parser");
            if (software is not null)
            {
                using var report = engine.ProbeSoftwareEncoder(software);
                Check(report.RootElement.GetProperty("framesSubmitted").GetInt32() == 60 && report.RootElement.GetProperty("packets").GetInt32() == 60, "Software encoder fixture failed.");
                cases.Add("real-software-h264-generated-fixture");
            }
            engine.Dispose(); engine.Dispose();
            bool disposedRejected = false;
            try { using var invalid = engine.GetCapabilities(); } catch (ObjectDisposedException) { disposedRejected = true; }
            Check(disposedRejected, "A released SafeHandle was used."); cases.Add("safehandle-double-dispose-use-after-dispose");
            for (int i = 0; i < 128; i++) Abandon(root);
            GC.Collect(); GC.WaitForPendingFinalizers(); GC.Collect();
            Check(NativeEngine.WaitForQuiescence(1000), "GC cleanup leaked native contexts/blobs."); cases.Add("gc-finalizers-real-native-quiescence");
            Type[] types = [typeof(nss_utf8_view), typeof(nss_version_v1), typeof(nss_error_v1), typeof(nss_json_v1), typeof(nss_event_v1),
                typeof(nss_preview_config_v1), typeof(nss_preview_frame_v1), typeof(nss_gpu_surface_v1), typeof(nss_packet_info_v1)];
            var layouts = types.ToDictionary(type => type.Name, type => new
            { size = Marshal.SizeOf(type), fields = type.GetFields(BindingFlags.Instance | BindingFlags.Public).ToDictionary(field => field.Name, field => (long)Marshal.OffsetOf(type, field.Name)) });
            var references = typeof(NativeEngine).Assembly.GetReferencedAssemblies().Select(a => a.Name).ToArray();
            Check(!references.Any(name => name is "PresentationFramework" or "PresentationCore" or "System.Windows.Forms" or "NessStudio"), "Interop depends on UI.");
            Console.WriteLine(JsonSerializer.Serialize(new { status = "PASS", build, cases, layouts, references, actual_native_contexts_finalized = true }, new JsonSerializerOptions { WriteIndented = true }));
            return 0;
        }
        catch (Exception error)
        {
            Console.WriteLine(JsonSerializer.Serialize(new { status = "FAIL", error = error.GetType().Name, message = error.Message }));
            return 2;
        }
    }
}

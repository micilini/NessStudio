using System.Diagnostics;
using System.Text.Json;
using System.Text.Json.Serialization;
using NessStudio.Interop.Native;
using NessStudio.Recording;
using NessStudio.Recording.Engines;

internal static class Program
{
    private static async Task<int> Main(string[] args)
    {
        if (args is ["--contracts"]) return SelectionContracts.Run();
        if (args is ["--thumbnail-contracts", var package, var normal, var shortClip, var thumbnailOutput])
            return await ThumbnailContracts.RunAsync(package, normal, shortClip, thumbnailOutput);
        if (args is not [var runtimeDirectory, var requestPath]) return 2;
        var options = new JsonSerializerOptions { PropertyNameCaseInsensitive = true };
        options.Converters.Add(new JsonStringEnumConverter());
        try
        {
            var request = JsonSerializer.Deserialize<RecordingRequest>(await File.ReadAllBytesAsync(requestPath), options)
                ?? throw new InvalidDataException("A recording request is required.");
            if (Directory.Exists(request.OutputDirectory))
                throw new IOException("Preserve existing evidence; use a fresh output directory.");
            Directory.CreateDirectory(request.OutputDirectory);
            var states = new List<RecordingState>();
            RecordingResult result;
            string[] preparedModules;
            await using (var service = new NativeRecorderEngineFactory(runtimeDirectory).Create(request))
            {
                service.StateChanged += (_, change) => { lock (states) states.Add(change.State); };
                await service.PrepareAsync().WaitAsync(TimeSpan.FromSeconds(20));
                using (var process = Process.GetCurrentProcess())
                    preparedModules = process.Modules.Cast<ProcessModule>().Select(module => module.ModuleName)
                        .Order(StringComparer.OrdinalIgnoreCase).ToArray();
                // Camera enumeration on this Windows build loads MFCORE/MFPlat
                // through the system DirectShow stack. Preserve the complete
                // module evidence; camera-free software must remain independent.
                if (!request.Targets.CaptureWebcam && request.ExecutionMode == RecordingExecutionMode.FullSoftware &&
                    preparedModules.Any(name => name.Equals("mf.dll", StringComparison.OrdinalIgnoreCase) ||
                        name.Equals("mfplat.dll", StringComparison.OrdinalIgnoreCase) || name.Equals("mfreadwrite.dll", StringComparison.OrdinalIgnoreCase)))
                    throw new InvalidDataException("The camera-free software route loaded a Media Foundation runtime.");
                var selected = service.Selection ?? throw new InvalidDataException("Preparation did not report a selection.");
                if (selected.EffectiveMode == RecordingExecutionMode.Automatic ||
                    request.ExecutionMode != RecordingExecutionMode.Automatic && request.Targets.AnyVideo &&
                    selected.EffectiveMode != request.ExecutionMode)
                    throw new InvalidDataException("Preparation did not honor the requested execution policy.");
                if (request.Video is not null && selected.Video != request.Video)
                    throw new InvalidDataException("Preparation silently changed explicit output geometry or bitrate.");
                await service.StartAsync().WaitAsync(TimeSpan.FromSeconds(10));
                await Task.Delay(1000);
                await service.PauseAsync().WaitAsync(TimeSpan.FromSeconds(10));
                // Let the bounded observer sample the established pause boundary.
                await Task.Delay(250);
                var cut = service.Statistics.ActiveDuration;
                await Task.Delay(350);
                if (service.Statistics.ActiveDuration != cut || service.State != RecordingState.Paused)
                    throw new InvalidDataException("Managed paused time advanced.");
                await service.ResumeAsync().WaitAsync(TimeSpan.FromSeconds(10));
                await Task.Delay(3000);
                string output = await service.StopAndFinalizeAsync().WaitAsync(TimeSpan.FromSeconds(45));
                result = service.Result ?? throw new InvalidDataException("Finalization did not publish a result.");
                if (output != result.PrimaryAssetPath || !File.Exists(output) || !File.Exists(result.ManifestPath) ||
                    service.State != RecordingState.Completed || result.Selection != selected)
                    throw new InvalidDataException("Managed finalization did not preserve its approved selection and assets.");
                var manifest = ProjectManifestStore.Read(request.OutputDirectory).Manifest;
                foreach (var track in new[] { manifest.HasScreen ? manifest.Screen : null,
                    manifest.HasWebcam ? manifest.Webcam : null, manifest.HasProgram ? manifest.Program : null })
                    if (track is not null && track.Fps != request.RuntimeOptions.RecordingFps)
                        throw new InvalidDataException("The project changed the requested output cadence.");
                bool program = request.Targets.AnyVideo && request.OutputMode != RecordingOutputMode.SeparateSources;
                bool raw = request.OutputMode != RecordingOutputMode.Program;
                if (manifest.HasProgram != program || manifest.HasScreen != (raw && request.Targets.CaptureScreen) ||
                    manifest.HasWebcam != (raw && request.Targets.CaptureWebcam))
                    throw new InvalidDataException("The published project does not match the requested source/program assets.");
            }
            if (!NativeEngine.WaitForQuiescence(1000))
                throw new InvalidDataException("Managed recording retained native resources after disposal.");
            Console.WriteLine(JsonSerializer.Serialize(new { status = "PASS", states, result, preparedModules, nativeQuiescence = true }, options));
            return 0;
        }
        catch (Exception error)
        {
            Console.WriteLine(JsonSerializer.Serialize(new { status = "FAIL", error = error.ToString() }));
            return 1;
        }
    }
}

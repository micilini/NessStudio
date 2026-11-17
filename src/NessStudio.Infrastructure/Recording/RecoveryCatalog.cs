using System.Diagnostics;
using System.Text.Json;
using NessStudio.Interop.Native;
using NessStudio.Models;
using NessStudio.Recording.Engines;
using NessStudio.ViewModel.Helpers;

namespace NessStudio.Recording;

/// <summary>A read-only diagnosis; discovery never modifies a manifest or registers a project.</summary>
public sealed record RecoveryCandidate(string Directory, string EvidencePath, string Status, string Message, bool CanRecover)
{
    public IReadOnlyList<RecoveryAsset> AvailableAssets { get; init; } = [];
    public string? FirstError { get; init; }
}

/// <summary>Describes an existing file without certifying an unfinished tail as recovered content.</summary>
public sealed record RecoveryAsset(string File, long Bytes, bool NativeFinalized);

/// <summary>Finds interrupted native operations and recovers only finalized, revalidated media into a new project.</summary>
public static class RecoveryCatalog
{
    private const int MaximumDirectories = 512;
    private const int MaximumFilesPerDirectory = 128;
    private static readonly JsonSerializerOptions Json = new() { PropertyNameCaseInsensitive = true, WriteIndented = true };

    private static JsonDocument Read(string path, int maximumBytes = 2097152)
    {
        using var file = new FileStream(path, FileMode.Open, FileAccess.Read, FileShare.ReadWrite | FileShare.Delete);
        if (file.Length is <= 0 || file.Length > maximumBytes) throw new InvalidDataException("Recovery metadata exceeds its size limit.");
        return JsonDocument.Parse(file, new() { MaxDepth = 64 });
    }

    private static bool Reparse(string path) => (File.GetAttributes(path) & FileAttributes.ReparsePoint) != 0;
    private static void CheckAssets(string directory, SessionManifest manifest)
    {
        foreach (var asset in ProjectManifestStore.Assets(manifest))
        {
            string path = ProjectManifestStore.ResolvePath(directory, asset.File);
            if (!File.Exists(path)) throw new InvalidDataException("Missing finalized asset: " + asset.File);
            if (asset.Bytes is { } bytes && new FileInfo(path).Length != bytes)
                throw new InvalidDataException("A finalized asset was changed or truncated: " + asset.File);
        }
    }

    private static bool OwnerBusy(JsonElement journal)
    {
        var snapshot = journal.GetProperty("snapshot");
        int state = snapshot.GetProperty("state").GetInt32();
        bool terminal = state is 10 or 11 or 104 or 105 or 106;
        if (terminal && !snapshot.GetProperty("cleanupPending").GetBoolean()) return false;
        try
        {
            using var process = Process.GetProcessById(journal.GetProperty("processId").GetInt32());
            return !process.HasExited && process.StartTime.ToUniversalTime().ToFileTimeUtc() == journal.GetProperty("processStartedFileTimeUtc").GetInt64();
        }
        catch (ArgumentException) { return false; }
        catch (InvalidOperationException) { return false; }
        catch (System.ComponentModel.Win32Exception) { return true; }
    }

    /// <summary>Scans a bounded recording tree without following junctions, loading media, writing data or opening devices.</summary>
    public static Task<IReadOnlyList<RecoveryCandidate>> ScanAsync(string root, CancellationToken cancellationToken = default) =>
        Task.Run<IReadOnlyList<RecoveryCandidate>>(() => Scan(root, cancellationToken), cancellationToken);

    private static IReadOnlyList<RecoveryCandidate> Scan(string root, CancellationToken cancellationToken)
    {
        var result = new List<RecoveryCandidate>();
        if (!Directory.Exists(root) || Reparse(root)) return result;
        var queue = new Queue<(string Directory, int Depth)>();
        queue.Enqueue((Path.GetFullPath(root), 0));
        int visited = 0;
        while (queue.Count != 0 && visited++ < MaximumDirectories)
        {
            cancellationToken.ThrowIfCancellationRequested();
            var (directory, depth) = queue.Dequeue();
            try
            {
                bool projectExists = File.Exists(Path.Combine(directory, "manifest.json")) || File.Exists(Path.Combine(directory, "session.manifest.json"));
                if (projectExists)
                {
                    try
                    {
                        var project = ProjectManifestStore.Read(directory).Manifest;
                        CheckAssets(directory, project);
                        string registration = Path.Combine(directory, "project-registration.json");
                        if (project.Version == 3 && File.Exists(registration))
                        {
                            using var receipt = Read(registration);
                            if (receipt.RootElement.GetProperty("version").GetInt32() == 1 && !receipt.RootElement.GetProperty("registered").GetBoolean())
                                result.Add(new(directory, registration, "RegistrationPending", "Finalized project files are available, but library registration did not finish. Registering again preserves its identity and avoids duplicates.", false));
                        }
                    }
                    catch (Exception error) when (Controlled(error))
                    { result.Add(new(directory, Path.Combine(directory, "manifest.json"), "ProjectNeedsAttention", error.Message, false)); }
                }
                var journals = Directory.EnumerateFiles(directory, "*.json", SearchOption.TopDirectoryOnly).Take(MaximumFilesPerDirectory * 2)
                    .Where(path => Path.GetFileName(path).Contains(".session-", StringComparison.Ordinal) || Path.GetFileName(path).Contains(".job-", StringComparison.Ordinal))
                    .Take(MaximumFilesPerDirectory).ToArray();
                foreach (string journal in journals)
                {
                    cancellationToken.ThrowIfCancellationRequested();
                    var candidate = Inspect(journal);
                    if (candidate is not null && (!projectExists || candidate.Status is "InterruptedExport" or "InvalidJournal" or "InUse")) result.Add(candidate);
                }
                if (!projectExists && journals.Length == 0 && ProjectRecoveryService.IsAvailable(directory))
                    result.Add(new(directory, Path.Combine(directory, "project-recovery.json"), "FinalizedProjectPending", "Finalized media has an unresolved project declaration. Recovering validates and copies the selected media.", true));
                if (depth < 3)
                    foreach (string child in Directory.EnumerateDirectories(directory).Take(MaximumDirectories - visited - queue.Count))
                        if (!Reparse(child)) queue.Enqueue((child, depth + 1));
            }
            catch (Exception error) when (Controlled(error))
            { result.Add(new(directory, directory, "ScanDiagnostic", error.Message, false)); }
        }
        if (queue.Count != 0) result.Add(new(root, root, "ScanLimit", "Recovery scan reached its 512-directory limit. Open the affected recording folder directly.", false));
        return result;
    }

    private static bool Controlled(Exception error) => error is IOException or InvalidDataException or JsonException or UnauthorizedAccessException or InvalidOperationException or ArgumentException or KeyNotFoundException or NotSupportedException or FormatException;

    private static IReadOnlyList<RecoveryAsset> Available(string directory)
    {
        var result=new List<RecoveryAsset>();
        foreach(string path in Directory.EnumerateFiles(directory).Take(MaximumFilesPerDirectory*2))
        {
            if(Path.GetExtension(path).ToLowerInvariant() is not (".mkv" or ".mp4" or ".wav")||Reparse(path))continue;
            long bytes=new FileInfo(path).Length;bool finalized=false;
            string sidecar=path+".manifest.json";
            if(File.Exists(sidecar))
            {
                try{using var document=Read(sidecar);var root=document.RootElement;finalized=root.GetProperty("finalized").GetBoolean()&&root.GetProperty("mediaComplete").GetBoolean()&&root.GetProperty("bytes").GetInt64()==bytes;}
                catch(Exception error)when(Controlled(error)){ }
            }
            result.Add(new(Path.GetFileName(path),bytes,finalized));
        }
        return result;
    }

    /// <summary>Inspects one known journal. A live owner or unsupported schema never permits recovery.</summary>
    public static RecoveryCandidate? Inspect(string path)
    {
        string directory = Path.GetDirectoryName(Path.GetFullPath(path))!;
        try
        {
            if (Reparse(path)) throw new InvalidDataException("Recovery does not follow journal links.");
            using var document = Read(path);
            var journal = document.RootElement;
            if (journal.GetProperty("version").GetInt32() != 1 || journal.GetProperty("kind").GetString() != "nessstudio.operation-journal")
                throw new InvalidDataException("Unsupported operation journal. Keep the files and use a compatible version.");
            if (OwnerBusy(journal)) return new(directory, path, "InUse", "The recording or export still belongs to a running process. Recovery is disabled until its resources are released.", false);
            var snapshot = journal.GetProperty("snapshot");
            RecoveryCandidate Candidate(string status,string message,bool recover)=>new(directory,path,status,message,recover)
            {
                AvailableAssets=Available(directory),
                FirstError=snapshot.TryGetProperty("firstError",out var error)&&error.ValueKind==JsonValueKind.Object&&error.TryGetProperty("message",out var description)?description.GetString():null
            };
            if (journal.GetProperty("operation").GetString() == "export")
            {
                string output = snapshot.GetProperty("outputPath").GetString()!;
                if (snapshot.GetProperty("mediaComplete").GetBoolean() && File.Exists(output) &&
                    snapshot.TryGetProperty("outputBytes", out var size) && size.ValueKind == JsonValueKind.Number && new FileInfo(output).Length == size.GetInt64() &&
                    File.Exists(snapshot.GetProperty("receiptPath").GetString())) return null;
                return Candidate("InterruptedExport", "An export was interrupted. Source media and any output or partial remain available; a partial is not certified as a recovered file. Export again to a new destination.", false);
            }
            if (journal.GetProperty("operation").GetString() != "recording") throw new InvalidDataException("Unsupported journal operation.");
            if (Resolved(directory, path)) return null;
            if (!snapshot.GetProperty("mediaComplete").GetBoolean())
                return Candidate("InterruptedCapture", "The native recording did not finalize. Available files are preserved; this version does not certify or automatically recover unfinished MKV/RF64 data.", false);
            _ = NativeManifest(directory, snapshot);
            return Candidate("FinalizedMediaPending", "Native media finalized before the project was saved. Recovery copies and fully decodes the available finalized tracks into a new project.", true);
        }
        catch (Exception error) when (Controlled(error))
        { return new(directory, path, "InvalidJournal", error.Message, false); }
    }

    private static bool Resolved(string directory, string journal)
    {
        string receipt = Path.Combine(directory, "recovery-result.json");
        if (!File.Exists(receipt)) return false;
        try
        {
            using var document = Read(receipt);
            var root = document.RootElement;
            if (root.GetProperty("version").GetInt32() != 1 || root.GetProperty("journal").GetString() != Path.GetFileName(journal)) return false;
            string recovered = ProjectManifestStore.ResolvePath(directory, root.GetProperty("recoveredDirectory").GetString()!);
            if (Path.GetDirectoryName(recovered) != directory || !Path.GetFileName(recovered).StartsWith("recovered-", StringComparison.Ordinal) || Reparse(recovered)) return false;
            var manifest = ProjectManifestStore.Read(recovered).Manifest;
            CheckAssets(recovered, manifest);
            return manifest.ProjectId == root.GetProperty("projectId").GetString();
        }
        catch (Exception error) when (Controlled(error)) { return false; }
    }

    private static SessionManifest NativeManifest(string directory, JsonElement snapshot)
    {
        var request = snapshot.GetProperty("requested");
        bool screen = request.TryGetProperty("capture", out _), camera = request.TryGetProperty("camera", out _);
        string mode = request.TryGetProperty("recordingMode", out var value) ? value.GetString()! : "SeparateSources";
        if (mode is not ("SeparateSources" or "Program" or "ProgramAndSources")) throw new InvalidDataException("Unknown native recording mode.");
        string? microphone = null, system = null;
        if (request.TryGetProperty("audio", out var audio))
            foreach (var track in audio.EnumerateArray())
            {
                string kind = track.GetProperty("kind").GetString()!;
                if (kind == "microphone") microphone = track.GetProperty("endpointId").GetString();
                else if (kind == "loopback") system = track.GetProperty("endpointId").GetString();
                else throw new InvalidDataException("Unknown native audio role.");
            }
        var monitor = screen ? new MonitorDescriptor("Recorded source metadata", new PhysicalRectangle(0, 0, 2, 2), false) : null;
        var targets = new RecordingTargets(monitor, camera ? "Recorded camera metadata" : null, microphone, system);
        bool program = targets.AnyVideo && mode != "SeparateSources", raw = mode != "Program";
        string primary = program ? "program.mkv" : screen ? "screen.mkv" : camera ? "webcam.mkv" : "audio.native.json";
        string sidecar = targets.AnyVideo ? primary + ".manifest.json" : primary;
        var manifest = NativeSessionManifestReader.Read(Path.Combine(directory, sidecar), directory, targets, camera: !program && !screen && camera, program: program);
        if (program && raw && screen)
        {
            var other = NativeSessionManifestReader.Read(Path.Combine(directory, "screen.mkv.manifest.json"), directory, new(monitor, null, null, null));
            SameCut(manifest, other); manifest.Screen = other.Screen; manifest.HasScreen = true;
        }
        if (raw && camera && (program || screen))
        {
            var other = NativeSessionManifestReader.Read(Path.Combine(directory, "webcam.mkv.manifest.json"), directory, new(null, "Recorded camera metadata", null, null), camera: true);
            SameCut(manifest, other); manifest.Webcam = other.Webcam; manifest.HasWebcam = true;
        }
        manifest.PrimaryAsset = targets.AnyVideo ? primary : manifest.Mic?.File ?? manifest.System?.File;
        manifest.RecordingMode = mode;
        manifest.CreatedAt = DateTimeOffset.UtcNow;
        manifest.UnknownFields = new() { ["RecoveryNativeJournalSnapshot"] = snapshot.Clone() };
        return manifest;
    }

    private static void SameCut(SessionManifest first, SessionManifest second)
    { if (Math.Abs(first.DurationMs!.Value - second.DurationMs!.Value) > 34) throw new InvalidDataException("Finalized native tracks have different cuts."); }

    /// <summary>Copies and decodes finalized tracks through native jobs, then publishes a recovered project; original files are retained.</summary>
    public static async Task<string> RecoverAsync(RecoveryCandidate candidate, IProgress<string>? progress = null, CancellationToken cancellationToken = default)
    {
        ArgumentNullException.ThrowIfNull(candidate);
        string directory = Path.GetFullPath(candidate.Directory);
        using var ownership = new FileStream(Path.Combine(directory, ".recovery-lock"), FileMode.OpenOrCreate, FileAccess.ReadWrite, FileShare.None);
        if (File.Exists(Path.Combine(directory, "manifest.json")) || File.Exists(Path.Combine(directory, "session.manifest.json")))
            throw new InvalidOperationException("A project already exists. Repair its aliases or locate its assets instead of replacing it.");
        SessionManifest manifest;
        if (Path.GetFileName(candidate.EvidencePath) == "project-recovery.json")
        {
            using var declaration = Read(candidate.EvidencePath);
            if (declaration.RootElement.GetProperty("version").GetInt32() != 1 || declaration.RootElement.GetProperty("resolved").GetBoolean()) throw new InvalidDataException("No unresolved project declaration.");
            manifest = ProjectManifestStore.Parse(System.Text.Encoding.UTF8.GetBytes(declaration.RootElement.GetProperty("manifest").GetRawText()));
            CheckAssets(directory, manifest);
        }
        else
        {
            var inspected = Inspect(candidate.EvidencePath);
            if (inspected is not { CanRecover: true } || Path.GetFullPath(inspected.Directory) != directory) throw new InvalidOperationException(inspected?.Message ?? "The operation was already recovered.");
            using var journal = Read(candidate.EvidencePath);
            manifest = NativeManifest(directory, journal.RootElement.GetProperty("snapshot"));
        }
        cancellationToken.ThrowIfCancellationRequested();
        using var engine = NativeEngine.Open(AppContext.BaseDirectory, "NessStudio.Recovery");
        if (engine.Build.AbiMinor < 8) throw new NotSupportedException("Recording recovery requires libnessstudio ABI 1.8 or newer.");
        string workspace = Path.Combine(directory, "recovered-" + Guid.NewGuid().ToString("N"));
        string plan = Path.Combine(workspace, "source-description"), assets = Path.Combine(workspace, "assets");
        Directory.CreateDirectory(plan);
        foreach (var asset in ProjectManifestStore.Assets(manifest))
        {
            string absolute = ProjectManifestStore.ResolvePath(directory, asset.File);
            asset.File = absolute;
            if (asset.Video is not null) asset.Video.File = absolute;
            if (asset.Audio is not null) asset.Audio.File = absolute;
        }
        manifest.PrimaryAsset = null;
        if (manifest.Version != 3) ProjectManifestStore.UpgradeNewRecording(manifest, directory, "libnessstudio/recovery/" + engine.Build.Build);
        manifest.Scene = null;
        manifest.PrimaryAsset = null;
        ProjectManifestStore.WriteAliases(plan, manifest);
        var copied = await ExportService.ExportAsync(new()
        {
            ProjectFolder = plan, OutputPath = assets, Mode = RecordingExportMode.SeparateTracks,
            ExportScreenTrack = manifest.HasScreen, ExportWebcamTrack = manifest.HasWebcam, ExportProgramTrack = manifest.HasProgram,
            ExportMicrophoneTrack = manifest.HasMic, ExportSystemAudioTrack = manifest.HasSystemAudio, ExportAudioMixTrack = false
        }, progress, cancellationToken).ConfigureAwait(false);
        if (!copied.Success) throw new InvalidDataException("Recovery did not complete; original and copied partial files were preserved. " + copied.Message);
        var names = new Dictionary<string, string> { ["screen"] = "screen.mp4", ["camera"] = "webcam.mp4", ["program"] = "program.mp4", ["microphone"] = "microphone.wav", ["system"] = "system.wav" };
        manifest.Assets.RemoveAll(asset => asset.Role == "primary");
        foreach (var asset in manifest.Assets)
        {
            asset.File = Path.Combine("assets", names[asset.Role]); asset.Bytes = new FileInfo(Path.Combine(workspace, asset.File)).Length;
            if (asset.Video is not null) { asset.Video.File = asset.File; asset.Video.ContainerKind = "mp4"; asset.Container = "mp4"; }
            if (asset.Audio is not null) asset.Audio.File = asset.File;
        }
        manifest.PrimaryAsset = manifest.Program?.File ?? manifest.Screen?.File ?? manifest.Webcam?.File ?? manifest.Mic?.File ?? manifest.System?.File;
        manifest.ProjectId = Guid.NewGuid().ToString("N");
        manifest.State = "Recovered"; manifest.Recoverable = false; manifest.FinalizationResult = "CopiedAndFullyDecodedFinalizedAssets";
        manifest.InterruptionCause = "The previous recording process ended before project publication or registration completed.";
        manifest.Errors.Add("Only finalized, independently revalidated track content was recovered; no unfinished MKV/RF64 tail is certified.");
        manifest.BaseDir = workspace;
        manifest.Thumbnail = null;
        ProjectManifestStore.WriteAliases(workspace, manifest);
        ProjectManifestStore.RecordRegistration(workspace, manifest.ProjectId!, false, "Recovered media is validated; library registration is pending.");
        ProjectManifestStore.WriteAtomic(Path.Combine(directory, "recovery-result.json"), JsonSerializer.SerializeToUtf8Bytes(new
        {
            version = 1, journal = Path.GetFileName(candidate.EvidencePath), recoveredDirectory = Path.GetRelativePath(directory, workspace),
            projectId = manifest.ProjectId, recoveredDurationMs = manifest.DurationMs, sourceAssetsPreserved = true,
            policy = "Only finalized track copies that passed native full decoding; no claim that all capture frames were recovered"
        }, Json), overwrite: true);
        if (Path.GetFileName(candidate.EvidencePath) == "project-recovery.json") ProjectRecoveryService.Resolve(directory, manifest.ProjectId!);
        return workspace;
    }
}

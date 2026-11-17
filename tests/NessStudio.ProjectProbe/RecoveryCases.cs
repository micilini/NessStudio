using System.Diagnostics;
using System.Text.Json;
using NessStudio.Recording;

internal static partial class Program
{
    private static async Task CaseAsync(string name, Func<Task> action) { await action(); Cases.Add(name); }
    private static string Journal(string folder, object snapshot, string operation = "recording", int version = 1, int? pid = null, long? started = null)
    {
        string path = Path.Combine(folder, operation == "recording" ? "screen.mkv.session-42.json" : "output.mp4.job-42.json");
        File.WriteAllText(path, JsonSerializer.Serialize(new { version, kind = "nessstudio.operation-journal", operation,
            processId = pid ?? int.MaxValue, processStartedFileTimeUtc = started ?? 1, snapshot }));
        return path;
    }
    private static object Snapshot(int state = 4, bool complete = false, bool cleanup = true) => new
    { state, mediaComplete = complete, cleanupPending = cleanup, requested = new { capture = new { version = 1 } } };
    private static string NativeCompleted(string folder)
    {
        string asset = Path.Combine(folder, "screen.mkv"); File.WriteAllBytes(asset, new byte[128]);
        File.WriteAllText(asset + ".manifest.json", JsonSerializer.Serialize(new
        {
            version = 1, mediaComplete = true, finalized = true, firstError = (object?)null, codec = "h264", container = "matroska", outputPath = asset,
            frames = 90, packets = 90, bytes = 128, durationHns = 30000000, sessionId = 42,
            encoder = new { width = 640, height = 360, fpsNum = 30, fpsDen = 1 }, audio = Array.Empty<object>()
        }));
        return Journal(folder, Snapshot(10, true, false));
    }
    private static async Task Recovery()
    {
        await CaseAsync("discovery-missing-root-never-creates-data", async () =>
        {
            string root = Path.Combine(_root, "absent scan root");
            Check((await RecoveryCatalog.ScanAsync(root)).Count == 0 && !Directory.Exists(root), "Discovery created a root");
        });
        Case("corrupt-journal-preserves-original-bytes", () =>
        {
            string root = Folder("broken journal"), path = Path.Combine(root, "screen.mkv.session-1.json"); File.WriteAllText(path, "{broken");
            Check(RecoveryCatalog.Inspect(path) is { Status: "InvalidJournal", CanRecover: false } && File.ReadAllText(path) == "{broken", "Corrupt journal was rewritten or accepted");
        });
        Case("future-journal-schema-rejected", () => Check(RecoveryCatalog.Inspect(Journal(Folder("future journal"), Snapshot(), version: 99)) is { Status: "InvalidJournal", CanRecover: false }, "A future journal was interpreted"));
        Case("journal-control-size-budget", () =>
        {
            string path = Path.Combine(Folder("huge journal"), "screen.mkv.session-1.json"); File.WriteAllBytes(path, new byte[2097153]);
            Check(RecoveryCatalog.Inspect(path) is { Status: "InvalidJournal", CanRecover: false }, "Journal budget was not enforced");
        });
        Case("journal-missing-owner-fields-controlled", () =>
        {
            string path = Path.Combine(Folder("missing owner"), "screen.mkv.session-1.json"); File.WriteAllText(path, "{\"version\":1,\"kind\":\"nessstudio.operation-journal\",\"operation\":\"recording\",\"snapshot\":{\"state\":4,\"cleanupPending\":true}}");
            Check(RecoveryCatalog.Inspect(path) is { Status: "InvalidJournal", CanRecover: false }, "Malformed ownership escaped diagnosis");
        });
        using var current = Process.GetCurrentProcess();
        long started = current.StartTime.ToUniversalTime().ToFileTimeUtc();
        Case("live-owner-never-offers-recovery", () => Check(RecoveryCatalog.Inspect(Journal(Folder("live owner"), Snapshot(), pid: current.Id, started: started)) is { Status: "InUse", CanRecover: false }, "Live owner was treated as abandoned"));
        Case("pid-reuse-compares-process-creation-time", () => Check(RecoveryCatalog.Inspect(Journal(Folder("reused pid"), Snapshot(), pid: current.Id, started: started + 1)) is { Status: "InterruptedCapture", CanRecover: false }, "PID reuse was mistaken for original ownership"));
        Case("terminal-but-cleanup-pending-retains-owner-guard", () => Check(RecoveryCatalog.Inspect(Journal(Folder("pending cleanup"), Snapshot(11), pid: current.Id, started: started)) is { Status: "InUse", CanRecover: false }, "Pending cleanup resources were exposed to recovery"));
        Case("unfinished-mkv-is-not-certified", () =>
        {
            string root = Folder("unfinished capture"), asset = Path.Combine(root, "screen.mkv"); File.WriteAllText(asset, "partial source bytes");
            string before = Hash(asset); Check(RecoveryCatalog.Inspect(Journal(root, Snapshot())) is { Status: "InterruptedCapture", CanRecover: false } && Hash(asset) == before, "Unfinalized media was certified or modified");
        });
        Case("finalized-journal-needs-actual-native-sidecar", () => Check(RecoveryCatalog.Inspect(Journal(Folder("missing native sidecar"), Snapshot(10, true, false))) is { Status: "InvalidJournal", CanRecover: false }, "A journal alone certified media"));
        Case("native-facts-only-enable-validation-attempt", () => Check(RecoveryCatalog.Inspect(NativeCompleted(Folder("native eligible metadata fixture"))) is { Status: "FinalizedMediaPending", CanRecover: true }, "Valid native facts did not enable the subsequent decode/copy validation step"));
        Case("truncated-native-file-blocks-recovery", () =>
        {
            string root = Folder("truncated native"), path = NativeCompleted(root); File.WriteAllBytes(Path.Combine(root, "screen.mkv"), new byte[64]);
            Check(RecoveryCatalog.Inspect(path) is { Status: "InvalidJournal", CanRecover: false }, "Truncated native media was accepted");
        });
        Case("unknown-operation-never-interpreted-as-capture", () => Check(RecoveryCatalog.Inspect(Journal(Folder("unknown operation"), Snapshot(), operation: "future-operation")) is { Status: "InvalidJournal", CanRecover: false }, "Unknown operation was interpreted"));
        Case("interrupted-export-keeps-output-and-partial", () =>
        {
            string root = Folder("interrupted export"), output = Path.Combine(root, "output.mp4"), partial = output + ".partial.mp4"; File.WriteAllText(partial, "keep partial");
            string path = Journal(root, new { state = 102, mediaComplete = false, cleanupPending = true, outputPath = output, partialPath = partial, receiptPath = output + ".export.json" }, "export");
            Check(RecoveryCatalog.Inspect(path) is { Status: "InterruptedExport", CanRecover: false } && File.ReadAllText(partial) == "keep partial", "Interrupted export modified its partial");
        });
        Case("complete-export-missing-file-still-diagnosed", () =>
        {
            string root = Folder("missing exported file"), output = Path.Combine(root, "output.mp4"); File.WriteAllText(output + ".export.json", "{}");
            string path = Journal(root, new { state = 105, mediaComplete = true, cleanupPending = false, outputPath = output, outputBytes = 128, receiptPath = output + ".export.json" }, "export");
            Check(RecoveryCatalog.Inspect(path) is { Status: "InterruptedExport", CanRecover: false }, "A receipt hid a missing export");
        });
        Case("complete-export-size-checked-at-reopen", () =>
        {
            string root = Folder("truncated exported file"), output = Path.Combine(root, "output.mp4"); File.WriteAllBytes(output, new byte[64]); File.WriteAllText(output + ".export.json", "{}");
            string path = Journal(root, new { state = 105, mediaComplete = true, cleanupPending = false, outputPath = output, outputBytes = 128, receiptPath = output + ".export.json" }, "export");
            Check(RecoveryCatalog.Inspect(path) is { Status: "InterruptedExport", CanRecover: false }, "Truncated export was hidden");
            File.WriteAllBytes(output, new byte[128]); Check(RecoveryCatalog.Inspect(path) is null, "Complete owned export produced a false orphan");
        });
        await CaseAsync("project-present-truncated-asset-diagnosed-without-rewrite", async () =>
        {
            string root = Folder("existing truncated project"); var manifest = V3(root); ProjectManifestStore.WriteAliases(root, manifest); string hash = Hash(Path.Combine(root, "manifest.json")); File.WriteAllBytes(Path.Combine(root, manifest.Screen.File), new byte[8]);
            Check((await RecoveryCatalog.ScanAsync(root)).Any(row => row.Status == "ProjectNeedsAttention" && !row.CanRecover) && Hash(Path.Combine(root, "manifest.json")) == hash, "Discovery rewrote a damaged project");
        });
        await CaseAsync("future-project-never-recovered-over-old-journal", async () =>
        {
            string root = Folder("future authority recovery"); NativeCompleted(root); File.WriteAllText(Path.Combine(root, "manifest.json"), "{\"Version\":99}");
            Check((await RecoveryCatalog.ScanAsync(root)).All(row => !row.CanRecover), "A future authoritative schema exposed old recovery");
        });
        Case("malformed-recovery-declaration-is-controlled", () =>
        {
            string root = Folder("malformed declaration"); File.WriteAllText(Path.Combine(root, "project-recovery.json"), "{}"); Check(!ProjectRecoveryService.IsAvailable(root), "Malformed recovery declaration escaped");
        });
        Case("metadata-recovery-rejects-changed-finalized-asset", () =>
        {
            string root = Folder("damaged declared asset"); var manifest = V3(root); ProjectRecoveryService.Stage(root, manifest); File.WriteAllBytes(Path.Combine(root, manifest.Screen.File), new byte[32]);
            Reject<InvalidDataException>(() => ProjectRecoveryService.Recover(root)); Check(!File.Exists(Path.Combine(root, "manifest.json")), "A damaged declaration was published as recovered");
        });
        await CaseAsync("startup-discovery-observes-cancellation", async () =>
        {
            using var cancel = new CancellationTokenSource(); cancel.Cancel();
            try { await RecoveryCatalog.ScanAsync(_root, cancel.Token); } catch (OperationCanceledException) { return; }
            throw new InvalidDataException("Cancelled startup scan completed normally");
        });
        await CaseAsync("pending-registration-detected-without-database-write",async()=>
        {
            string root=Folder("pending project registration");var manifest=V3(root);ProjectManifestStore.WriteAliases(root,manifest);ProjectManifestStore.RecordRegistration(root,manifest.ProjectId!,false,"Owned injected registration failure");
            Check((await RecoveryCatalog.ScanAsync(root)).Any(row=>row.Status=="RegistrationPending"&&!row.CanRecover),"Pending library registration was hidden or offered destructive recovery");
        });
    }
}

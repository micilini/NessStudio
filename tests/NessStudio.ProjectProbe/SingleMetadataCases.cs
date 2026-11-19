using System.Text.Json;
using NessStudio.Recording;

internal static partial class Program
{
    private static void SingleMetadataCases()
    {
        Case("single-json-combined-budget-accommodates-native-documents", () =>
        {
            string folder=Folder("single combined native documents");var manifest=V3(folder);
            string[] names=["screen.mkv.manifest.json","webcam.mkv.manifest.json","audio.native.json","recording-native.json"];
            foreach(string name in names)File.WriteAllText(Path.Combine(folder,name),JsonSerializer.Serialize(new { diagnostic=new string('x',300_000) }));
            ProjectMetadataStore.ConsolidateAsync(folder,manifest).GetAwaiter().GetResult();
            Check(new FileInfo(Path.Combine(folder,"manifest.json")).Length>1_048_576&&Directory.GetFiles(folder,"*.json").Length==1,
                "Consolidation retained the single-sidecar byte budget");
            foreach(string name in names)
            {
                using var restored=ProjectMetadataStore.ReadDocument(folder,name);
                Check(restored!.RootElement.GetProperty("diagnostic").GetString()!.Length==300_000,"Combined metadata lost native fields");
            }
        });
        Case("single-json-retains-native-documents-and-media", () =>
        {
            string folder = Folder("single metadata"); var manifest = V3(folder);
            string source = Path.Combine(folder, "screen.mkv.manifest.json");
            File.WriteAllText(source, "{\"version\":1,\"custom\":{\"counter\":18446744073709551615,\"values\":[1.25,true,null]}}");
            string media = Path.Combine(folder, manifest.Screen.File), before = Hash(media);
            ProjectMetadataStore.SetDocument(manifest,"recording-native.json",JsonSerializer.SerializeToElement(new { automaticExport = false }));
            ProjectMetadataStore.Consolidate(folder,manifest);
            Check(Directory.GetFiles(folder,"*.json").Select(Path.GetFileName).SequenceEqual(["manifest.json"]) && Hash(media)==before,
                "Consolidation changed source media or left multiple metadata files");
            using var restored=ProjectMetadataStore.ReadDocument(folder,"screen.mkv.manifest.json");
            Check(restored!.RootElement.GetProperty("custom").GetProperty("counter").GetUInt64()==ulong.MaxValue &&
                ProjectManifestStore.Read(folder).Manifest.Metadata!.Documents.ContainsKey("recording-native.json"),"Embedded metadata lost fields or numeric precision");
        });
        Case("single-json-keeps-unowned-user-documents", () =>
        {
            string folder=Folder("single user documents");var manifest=V3(folder);
            string note=Path.Combine(folder,"my-notes.json");File.WriteAllText(note,"{\"note\":\"preserve this\"}");string before=Hash(note);
            ProjectMetadataStore.Consolidate(folder,manifest);
            Check(Hash(note)==before && !ProjectManifestStore.Read(folder).Manifest.Metadata!.Documents.ContainsKey("my-notes.json"),"Consolidation consumed unowned user data");
        });
        Case("single-json-refuses-active-native-journal", () =>
        {
            string folder=Folder("single active journal");var manifest=V3(folder);
            string journal=Path.Combine(folder,"screen.mkv.session-1.json");
            File.WriteAllText(journal,"{\"kind\":\"nessstudio.operation-journal\",\"snapshot\":{\"cleanupPending\":true}}");
            Reject<InvalidOperationException>(()=>ProjectMetadataStore.Consolidate(folder,manifest));
            Check(File.Exists(journal)&&!File.Exists(Path.Combine(folder,"manifest.json")),"An active journal was retired");
        });
        Case("single-json-registration-remains-in-authority", () =>
        {
            string folder=Folder("single registration");var manifest=V3(folder);ProjectMetadataStore.Consolidate(folder,manifest);
            ProjectManifestStore.RecordRegistration(folder,manifest.ProjectId!,false,"injected database failure");
            using(var receipt=ProjectMetadataStore.ReadDocument(folder,"project-registration.json"))
                Check(!receipt!.RootElement.GetProperty("registered").GetBoolean(),"Pending registration was lost");
            var candidates=RecoveryCatalog.ScanAsync(folder).GetAwaiter().GetResult();
            Check(candidates.Any(candidate=>candidate.Status=="RegistrationPending"&&Path.GetFileName(candidate.EvidencePath)=="manifest.json"),"Embedded pending registration was not discovered");
            ProjectManifestStore.RecordRegistration(folder,manifest.ProjectId!,true);
            using var completed=ProjectMetadataStore.ReadDocument(folder,"project-registration.json");
            Check(completed!.RootElement.GetProperty("registered").GetBoolean()&&Directory.GetFiles(folder,"*.json").Length==1,"Registration created another metadata file");
        });
        Case("single-json-recovery-bookkeeping-is-embedded", () =>
        {
            string folder=Folder("single recovery state");var manifest=V3(folder);ProjectMetadataStore.Consolidate(folder,manifest);
            ProjectRecoveryService.Stage(folder,manifest,"injected project publication failure");
            Check(ProjectRecoveryService.IsAvailable(folder),"Embedded recovery declaration was not readable");
            ProjectRecoveryService.Resolve(folder,manifest.ProjectId!);
            Check(!ProjectRecoveryService.IsAvailable(folder)&&Directory.GetFiles(folder,"*.json").Length==1,"Recovery bookkeeping generated extra JSON files");
        });
        Case("single-json-project-survives-folder-relocation", () =>
        {
            string folder=Folder("single before move");var manifest=V3(folder);ProjectMetadataStore.Consolidate(folder,manifest);
            string moved=Path.Combine(_root,"single after move");Directory.Move(folder,moved);
            Check(ProjectManifestStore.Read(moved).MissingAssets.Count==0&&Directory.GetFiles(moved,"*.json").Length==1,"Moving a consolidated project broke relative source assets");
        });
        Case("single-json-rejects-unsupported-metadata-version", () =>
        {
            string folder=Folder("single future metadata");var manifest=V3(folder);manifest.Metadata=new(){Version=99};
            Reject<InvalidDataException>(()=>ProjectMetadataStore.Consolidate(folder,manifest));
            Check(!File.Exists(Path.Combine(folder,"manifest.json")),"Unsupported metadata was published");
        });
        Case("single-json-publication-failure-preserves-documents", () =>
        {
            string folder=Folder("single failed authority");var manifest=V3(folder);
            string native=Path.Combine(folder,"screen.mkv.manifest.json");File.WriteAllText(native,"{\"version\":1,\"keep\":true}");string before=Hash(native);
            Directory.CreateDirectory(Path.Combine(folder,"manifest.json"));
            Reject<IOException>(()=>ProjectMetadataStore.Consolidate(folder,manifest));
            Check(Hash(native)==before,"A failed canonical write removed the original native document");
        });
        Case("single-json-async-consolidation-preserves-active-checkpoint", () =>
        {
            string folder=Folder("single cancelled thumbnail");var manifest=V3(folder);
            string journal=Path.Combine(folder,"preview.png.job-1.json");
            File.WriteAllText(journal,"{\"kind\":\"nessstudio.operation-journal\",\"snapshot\":{\"cleanupPending\":true}}");
            Reject<InvalidOperationException>(()=>ProjectMetadataStore.ConsolidateAsync(folder,manifest).GetAwaiter().GetResult());
            Check(File.Exists(journal)&&!File.Exists(Path.Combine(folder,"manifest.json")),"An unfinished thumbnail checkpoint was retired");
            ProjectManifestStore.WriteAtomic(journal,System.Text.Encoding.UTF8.GetBytes("{\"kind\":\"nessstudio.operation-journal\",\"snapshot\":{\"cleanupPending\":false}}"),overwrite:true);
            ProjectMetadataStore.ConsolidateAsync(folder,manifest).GetAwaiter().GetResult();
            Check(Directory.GetFiles(folder,"*.json").Select(Path.GetFileName).SequenceEqual(["manifest.json"]),"Completed thumbnail checkpoint was not consolidated");
        });
    }
}

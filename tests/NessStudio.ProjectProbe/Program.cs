using System.Security.Cryptography;
using System.Text;
using System.Text.Json;
using NessStudio.Models;
using NessStudio.Recording;
using NessStudio.ViewModel.Helpers;
using SQLite;

internal static partial class Program
{
    private static readonly List<string> Cases=[];
    private static string _root=string.Empty;
    private static void Check(bool value,string message){if(!value)throw new InvalidDataException(message);}
    private static void Reject<T>(Action action)where T:Exception{try{action();}catch(T){return;}throw new InvalidDataException("Invalid contract was accepted: "+typeof(T).Name);}
    private static void Case(string name,Action action){action();Cases.Add(name);}
    private static byte[] Json(SessionManifest manifest)=>JsonSerializer.SerializeToUtf8Bytes(manifest);
    private static string Folder(string name){string path=Path.Combine(_root,name);Directory.CreateDirectory(path);return path;}
    private static SessionManifest V2(string folder)
    {
        File.WriteAllBytes(Path.Combine(folder,"screen custom & 'ç'.mkv"),new byte[128]);
        return new(){Version=2,SessionId="isolated-session",CreatedAt=DateTimeOffset.UtcNow,DurationMs=3000,BaseDir=folder,HasScreen=true,
            PrimaryAsset="screen custom & 'ç'.mkv",Screen=new(){File="screen custom & 'ç'.mkv",Width=640,Height=360,Fps=30,FrameCount=90,Duration="00:00:03",ContainerKind="matroska",PixelFormat="h264"}};
    }
    private static SessionManifest V3(string folder){var manifest=V2(folder);ProjectManifestStore.UpgradeNewRecording(manifest,folder,"libnessstudio/test-only");return manifest;}
    private static string Hash(string path)=>Convert.ToHexString(SHA256.HashData(File.ReadAllBytes(path)));
    private static void Manifests()
    {
        Case("v2-aliases-preserve-original-bytes",()=>
        {
            string folder=Folder("v2 aliases");var manifest=V2(folder);ProjectManifestStore.WriteAliases(folder,manifest);string before=Hash(Path.Combine(folder,"manifest.json"));
            var read=ProjectManifestStore.Read(folder);Check(read.Manifest.Version==2&&read.Manifest.Screen.File==manifest.Screen.File&&read.MissingAssets.Count==0,"V2 metadata was lost");
            Check(Hash(Path.Combine(folder,"manifest.json"))==before&&File.ReadAllBytes(Path.Combine(folder,"manifest.json")).SequenceEqual(File.ReadAllBytes(Path.Combine(folder,"session.manifest.json"))),"Opening rewrote the aliases");
        });
        Case("alias-only-fallback-without-rewrite",()=>{string folder=Folder("alias only");File.WriteAllBytes(Path.Combine(folder,"session.manifest.json"),Json(V2(folder)));var read=ProjectManifestStore.Read(folder);Check(Path.GetFileName(read.AuthorityPath)=="session.manifest.json"&&!File.Exists(Path.Combine(folder,"manifest.json")),"Alias-only read mutated the project");});
        Case("invalid-authority-valid-alias",()=>{string folder=Folder("invalid primary");File.WriteAllBytes(Path.Combine(folder,"session.manifest.json"),Json(V2(folder)));File.WriteAllText(Path.Combine(folder,"manifest.json"),"{broken");var read=ProjectManifestStore.Read(folder);Check(read.Manifest.Version==2&&read.Diagnostics.Count>0&&File.ReadAllText(Path.Combine(folder,"manifest.json"))=="{broken","Invalid authority fallback was unsafe");});
        Case("unsupported-authority-never-stale-fallback",()=>{string folder=Folder("future");File.WriteAllBytes(Path.Combine(folder,"session.manifest.json"),Json(V2(folder)));File.WriteAllText(Path.Combine(folder,"manifest.json"),"{\"Version\":99,\"Schema\":\"future\"}");Reject<UnsupportedProjectSchemaException>(()=>ProjectManifestStore.Read(folder));});
        Case("different-supported-versions-primary-authority",()=>{string folder=Folder("versions");File.WriteAllBytes(Path.Combine(folder,"manifest.json"),Json(V2(folder)));File.WriteAllBytes(Path.Combine(folder,"session.manifest.json"),Json(V3(folder)));var read=ProjectManifestStore.Read(folder);Check(read.Manifest.Version==2&&read.Diagnostics.Any(),"Primary authority was not preserved");});
        Case("invalid-v3-primary-fallback-v2",()=>{string folder=Folder("invalid v3");File.WriteAllBytes(Path.Combine(folder,"session.manifest.json"),Json(V2(folder)));File.WriteAllText(Path.Combine(folder,"manifest.json"),"{\"Version\":3,\"Schema\":\"wrong-role\"}");Check(ProjectManifestStore.Read(folder).Manifest.Version==2,"An invalid v3 was interpreted as media metadata");});
        Case("unknown-fields-roundtrip-and-explicit-repair",()=>{string folder=Folder("unknown");var m=V3(folder);m.UnknownFields=new(){["FutureLabel"]=JsonSerializer.SerializeToElement(new{label="retain me"})};m.Assets[0].UnknownFields=new(){["CustomMetadata"]=JsonSerializer.SerializeToElement(42)};ProjectManifestStore.WriteAliases(folder,m);File.WriteAllText(Path.Combine(folder,"session.manifest.json"),"{}");ProjectManifestStore.RepairAliases(folder);var read=ProjectManifestStore.Read(folder);Check(read.Manifest.UnknownFields!["FutureLabel"].GetProperty("label").GetString()=="retain me"&&read.Manifest.Assets[0].UnknownFields!["CustomMetadata"].GetInt32()==42,"Unknown project fields were lost");Check(Directory.GetFiles(folder,"*.before-repair").Length==2,"Explicit repair did not preserve both original aliases");});
        Case("v3-general-assets-override-stale-track-projection",()=>{string folder=Folder("assets authoritative");var m=V3(folder);m.Screen.File="obsolete-name.mkv";m.Assets[0].File="screen custom & 'ç'.mkv";ProjectManifestStore.WriteAliases(folder,m);var read=ProjectManifestStore.Read(folder);Check(read.Manifest.Screen.File==read.Manifest.Assets[0].File,"V3 reader trusted stale v2 projection");});
        Case("missing-asset-diagnosis-and-relocation",()=>{string folder=Folder("relocation");var m=V3(folder);ProjectManifestStore.WriteAliases(folder,m);string moved=Path.Combine(folder,"moved & ç.mkv");File.Move(Path.Combine(folder,m.Screen.File),moved);var read=ProjectManifestStore.Read(folder);Check(read.MissingAssets.Count==1&&Directory.Exists(folder),"Missing asset was not diagnosed");ProjectManifestStore.RelocateAsset(folder,"screen",moved);read=ProjectManifestStore.Read(folder);Check(read.MissingAssets.Count==0&&read.Manifest.Screen.File=="moved & ç.mkv"&&File.Exists(moved),"Relocation changed/lost the asset");});
        Case("wrong-size-relocation-rejected",()=>{string folder=Folder("wrong relocation");var m=V3(folder);ProjectManifestStore.WriteAliases(folder,m);string wrong=Path.Combine(folder,"wrong.mkv");File.WriteAllText(wrong,"bad");Reject<InvalidDataException>(()=>ProjectManifestStore.RelocateAsset(folder,"screen",wrong));});
        Case("relative-paths-survive-project-move",()=>{string folder=Folder("original location");var m=V3(folder);ProjectManifestStore.WriteAliases(folder,m);string moved=Path.Combine(_root,"moved project");Directory.Move(folder,moved);Check(ProjectManifestStore.Read(moved).MissingAssets.Count==0,"Historical BaseDir was treated as a dependency");});
        Case("duplicate-case-version-fields-rejected",()=>Reject<InvalidDataException>(()=>ProjectManifestStore.Parse(Encoding.UTF8.GetBytes("{\"Version\":2,\"version\":9}"))));
        Case("byte-and-depth-budgets",()=>{Reject<InvalidDataException>(()=>ProjectManifestStore.Parse(new byte[8*1024*1024+1]));Reject<JsonException>(()=>ProjectManifestStore.Parse(Encoding.UTF8.GetBytes("{\"Version\":2,\"extra\":"+new string('[',70)+"0"+new string(']',70)+"}")));});
        Case("duplicate-asset-roles-rejected",()=>{string folder=Folder("duplicate roles");var m=V3(folder);m.Assets.Add(m.Assets[0]);Reject<InvalidDataException>(()=>ProjectManifestStore.Parse(Json(m)));});
        Case("unfinalized-assets-rejected",()=>{string folder=Folder("partial role");var m=V3(folder);m.Assets[0].Finalized=false;Reject<InvalidDataException>(()=>ProjectManifestStore.Parse(Json(m)));});
        Case("future-session-schema-not-project",()=>Reject<UnsupportedProjectSchemaException>(()=>ProjectManifestStore.Parse(Encoding.UTF8.GetBytes("{\"version\":1,\"kind\":\"native-session\"}"))));
        Case("v3-requires-format-and-engine-identity",()=>{string folder=Folder("identity");var m=V3(folder);m.EngineVersion=null;Reject<InvalidDataException>(()=>ProjectManifestStore.Parse(Json(m)));m.EngineVersion="test";m.Assets[0].Video=null;Reject<InvalidDataException>(()=>ProjectManifestStore.Parse(Json(m)));});
        Case("manifest-write-failure-preserves-recovery-and-media",()=>
        {
            string folder=Folder("recovery");var m=V3(folder);string mediaHash=Hash(Path.Combine(folder,m.Screen.File));ProjectRecoveryService.Stage(folder,m,"injected manifest IO failure");Directory.CreateDirectory(Path.Combine(folder,"manifest.json"));Reject<IOException>(()=>ProjectManifestStore.WriteAliases(folder,m));Check(ProjectRecoveryService.IsAvailable(folder)&&Hash(Path.Combine(folder,m.Screen.File))==mediaHash,"Manifest failure lost recovery/media");
            Directory.Delete(Path.Combine(folder,"manifest.json"));ProjectRecoveryService.Recover(folder);Check(ProjectManifestStore.Read(folder).Manifest.State=="Recovered"&&!ProjectRecoveryService.IsAvailable(folder),"Explicit recovery did not produce an honest project state");
        });
        Case("future-authority-blocks-recovery-downgrade",()=>{string folder=Folder("recovery future");ProjectRecoveryService.Stage(folder,V3(folder));File.WriteAllText(Path.Combine(folder,"manifest.json"),"{\"Version\":99}");Reject<UnsupportedProjectSchemaException>(()=>ProjectRecoveryService.Recover(folder));});
        Case("owned-pending-file-collision-is-preserved",()=>{string folder=Folder("pending collision");var m=V3(folder);File.WriteAllText(Path.Combine(folder,"manifest.json.pending"),"belongs to an earlier attempt");ProjectManifestStore.WriteAliases(folder,m);Check(File.ReadAllText(Path.Combine(folder,"manifest.json.pending"))=="belongs to an earlier attempt","Writer deleted an unowned pending file");});
        Case("database-registration-failure-receipt",()=>{string folder=Folder("registration recovery");ProjectManifestStore.RecordRegistration(folder,"p-1",false,"injected database IO failure");using var read=JsonDocument.Parse(File.ReadAllText(Path.Combine(folder,"project-registration.json")));Check(read.RootElement.GetProperty("recoverable").GetBoolean()&&read.RootElement.GetProperty("sourceAssetsPreserved").GetBoolean(),"Database recovery state is dishonest");});
    }
    private static void Database()
    {
        SQLitePCL.raw.SetProvider(new SQLitePCL.SQLite3Provider_e_sqlcipher());
        string folder=Folder("database copies"),baseline=Path.Combine(folder,"baseline.sqlite"),copy=Path.Combine(folder,"working-copy.sqlite");
        const string ownFixtureKey="isolated-project-contract-fixture-key";
        using(var source=new SQLiteConnection(new SQLiteConnectionString(baseline,true,ownFixtureKey)))
        {
            source.CreateTable<ProjectsModel>();source.CreateTable<SettingsModel>();source.CreateTable<NessStudio.Models.AppVersion>();source.Insert(new NessStudio.Models.AppVersion{Version=2.0});
            source.Insert(new SettingsModel{ApplicationIdentifier="keep-id",Language="pt_br",EnableDebugLogs=true});source.Insert(new ProjectsModel{ProjectId="legacy-project",Title="Keep title & ç",ProjectFolderPath="C:/original missing fixture",Tags="preserve tags",IsFavorite=true});
        }
        string baselineHash=Hash(baseline);File.Copy(baseline,copy);
        using var connection=new SQLiteConnection(new SQLiteConnectionString(copy,true,ownFixtureKey));DatabaseHelper.Configure(()=>connection);
        string Schema()=>string.Join("\n",connection.Query<SchemaRow>("SELECT name,sql FROM sqlite_master WHERE type='table' ORDER BY name").Select(row=>row.name+row.sql));
        string schemaBefore=Schema();
        Case("encrypted-project-settings-appversion-copies",()=>{Check(!string.IsNullOrEmpty(connection.ExecuteScalar<string>("PRAGMA cipher_version")),"SQLCipher fixture is not encrypted");Check(connection.Table<ProjectsModel>().Count()==1&&connection.Table<SettingsModel>().Single().Language=="pt_br"&&connection.Table<NessStudio.Models.AppVersion>().Single().Version==2.0,"Baseline table copy lost values");});
        Case("registration-idempotent-preserves-title-path-and-flags",()=>{var row=DatabaseHelper.RegisterProject(new(){ProjectId="legacy-project",Title="Must not overwrite",ProjectFolderPath="C:/another missing fixture"});Check(row.Title=="Keep title & ç"&&row.Tags=="preserve tags"&&row.IsFavorite&&row.ProjectFolderPath=="C:/original missing fixture"&&connection.Table<ProjectsModel>().Count()==1,"Existing project was rewritten");});
        Case("transactional-new-project-registration",()=>{string assets=Folder("registered assets");var row=DatabaseHelper.RegisterProject(new(){ProjectId="native-project",Title="Native",ProjectFolderPath=assets});Check(row.Id>0&&connection.Table<ProjectsModel>().Count()==2,"Native registration did not commit");});
        Case("explicit-import-relinks-moved-project-location",()=>{string assets=Folder("relinked assets");var row=DatabaseHelper.RegisterProject(new(){ProjectId="legacy-project",Title="Do not replace",ProjectFolderPath=assets,ProjectFilePath=Path.Combine(assets,"manifest.json")});Check(row.Title=="Keep title & ç"&&row.ProjectFolderPath==assets,"Explicit relocation lost title or path");});
        Case("schema-settings-appversion-and-source-copy-preserved",()=>{Check(Schema()==schemaBefore&&connection.Table<SettingsModel>().Single().ApplicationIdentifier=="keep-id"&&connection.Table<NessStudio.Models.AppVersion>().Single().Version==2.0&&Hash(baseline)==baselineHash,"Registration changed schema/settings/version or baseline");});
    }
    private sealed class SchemaRow {public string name{get;set;}=string.Empty;public string sql{get;set;}=string.Empty;}
    private static void RecordingLevels()
    {
        Case("recording-level-preferences-migrate-and-preserve-selected-gains", () =>
        {
            var path = Path.Combine(Folder("recording levels"), "preferences.json");
            File.WriteAllText(path, "{\"TimerSeconds\":5,\"RecordingFps\":30}");
            var service = new RecordingPreferencesService(() => path);
            var levels = service.Load();
            Check(levels.MicrophoneLevel == 100 && levels.SystemAudioLevel == 100 && levels.TimerSeconds == 5,
                "Older preferences changed the captured audio level");
            levels.MicrophoneLevel = 25;
            levels.SystemAudioLevel = 0;
            service.Save(levels);
            var restored = service.Load();
            Check(restored.MicrophoneLevel == 25 && restored.SystemAudioLevel == 0 && restored.TimerSeconds == 5,
                "Selected levels were not retained independently of existing preferences");
            Check(RecordingPreferencesService.NormalizeLevel(double.NaN) == 100 &&
                RecordingPreferencesService.NormalizeLevel(-1) == 0 && RecordingPreferencesService.NormalizeLevel(101) == 100,
                "Invalid preference levels escaped the supported recording gain range");
        });
    }
    public static async Task<int> Main(string[] args)
    {
        try{if(args is not [var folder])return 2;_root=Path.GetFullPath(folder);if(Directory.Exists(_root))throw new IOException("Preserve existing project contract fixture; choose a fresh directory.");Directory.CreateDirectory(_root);EnglishErrorCases();RecordingLevels();RecordingStorageCases();Manifests();ProjectFilesCases();SingleMetadataCases();Database();DatabaseRuntime();await Recovery();Console.WriteLine(JsonSerializer.Serialize(new{status="PASS",cases=Cases.Count,names=Cases,scope="Versioned project/encrypted fixtures, pinned managed/native database runtime, single-file recording metadata, bounded crash discovery, owner guards and preserved/truncated media diagnostics; metadata eligibility is not a successful media recovery claim"}));return 0;}
        catch(Exception error){Console.Error.WriteLine(error);return 1;}
    }
}

using System.Text.Json;

namespace NessStudio.Recording;

/// <summary>Records a recoverable project declaration before publishing manifests; never deletes media or partial files.</summary>
public static class ProjectRecoveryService
{
    private static readonly JsonSerializerOptions Options = new() { WriteIndented = true };
    public static void Stage(string directory, SessionManifest manifest, string? error = null)
    {
        if (manifest.Metadata is { } metadata)
        {
            metadata.Recovery = JsonSerializer.SerializeToElement(new { version = 1, resolved = false, sourceAssetsPreserved = true, error }, Options);
            ProjectMetadataStore.Consolidate(directory, manifest);
            return;
        }
        byte[] bytes=JsonSerializer.SerializeToUtf8Bytes(new { version=1,resolved=false,sourceAssetsPreserved=true,error,manifest },Options);
        ProjectManifestStore.WriteAtomic(Path.Combine(directory,"project-recovery.json"),bytes,overwrite:true);
    }
    public static bool IsAvailable(string directory)
    {
        try { using var document=ProjectMetadataStore.ReadDocument(directory,"project-recovery.json");return document is not null&&document.RootElement.GetProperty("version").GetInt32()==1&&!document.RootElement.GetProperty("resolved").GetBoolean(); }
        catch(Exception error)when(error is IOException or InvalidDataException or JsonException or InvalidOperationException or KeyNotFoundException or FormatException){return false;}
    }
    private static JsonDocument Read(string path)
    { using var file=File.OpenRead(path);if(file.Length is <=0 or >1048576)throw new InvalidDataException("Recovery declaration exceeds its budget.");return JsonDocument.Parse(file,new(){MaxDepth=64}); }
    public static string Recover(string directory)
    {
        // Future authoritative schemas must never be downgraded using a stale recovery declaration.
        try { _=ProjectManifestStore.Read(directory);throw new InvalidOperationException("A readable project already exists; use explicit alias repair or asset relocation."); }
        catch(UnsupportedProjectSchemaException){throw;}
        catch(InvalidDataException){ }
        using var document=Read(Path.Combine(directory,"project-recovery.json"));var root=document.RootElement;
        if(root.GetProperty("version").GetInt32()!=1||root.GetProperty("resolved").GetBoolean())throw new InvalidDataException("No unresolved recovery declaration.");
        var manifest=ProjectManifestStore.Parse(System.Text.Encoding.UTF8.GetBytes(root.GetProperty("manifest").GetRawText()));
        if(ProjectManifestStore.Assets(manifest).Any(asset=>!File.Exists(ProjectManifestStore.ResolvePath(directory,asset.File))))throw new FileNotFoundException("Locate missing media before recovering the project.");
        foreach(var asset in ProjectManifestStore.Assets(manifest))
            if(asset.Bytes is { } bytes && new FileInfo(ProjectManifestStore.ResolvePath(directory,asset.File)).Length!=bytes)throw new InvalidDataException("A finalized asset was changed or truncated: "+asset.File);
        manifest.State="Recovered";manifest.Recoverable=false;manifest.FinalizationResult="RecoveredFinalizedAssets";
        if(root.TryGetProperty("error",out var error)&&error.ValueKind==JsonValueKind.String){manifest.InterruptionCause=error.GetString();manifest.Errors.Add(error.GetString()!);}
        string result=ProjectManifestStore.WriteAliases(directory,manifest,overwrite:true);Resolve(directory,manifest.ProjectId!);return result;
    }
    public static void Resolve(string directory,string projectId)
    {
        if (File.Exists(Path.Combine(directory,"manifest.json")))
        {
            var manifest = ProjectManifestStore.Read(directory).Manifest;
            if (manifest.Metadata is { } metadata)
            {
                metadata.Recovery = JsonSerializer.SerializeToElement(new {version=1,resolved=true,projectId,sourceAssetsPreserved=true},Options);
                ProjectManifestStore.WriteAliases(directory,manifest,overwrite:true);
                return;
            }
        }
        ProjectManifestStore.WriteAtomic(Path.Combine(directory,"project-recovery.json"),JsonSerializer.SerializeToUtf8Bytes(new {version=1,resolved=true,projectId,sourceAssetsPreserved=true},Options),overwrite:true);
    }
}

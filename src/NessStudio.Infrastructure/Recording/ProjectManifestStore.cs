using System.Globalization;
using System.Text.Json;

namespace NessStudio.Recording;

/// <summary>Owns version detection, manifest authority, atomic aliases and explicit project repair.</summary>
public static class ProjectManifestStore
{
    public const string Schema = "nessstudio.project";
    public const int CurrentVersion = 3;
    private const int MaximumBytes = 1048576;
    private static readonly JsonSerializerOptions Options = new() { PropertyNameCaseInsensitive = true, AllowTrailingCommas = true, WriteIndented = true, MaxDepth = 64 };

    public static ProjectManifestDocument Read(string directory)
    {
        directory = Path.GetFullPath(directory);
        var diagnostics = new List<string>();
        SessionManifest? selected = null; string? authority = null; byte[]? selectedBytes = null;
        foreach (string name in new[] { "manifest.json", "session.manifest.json" })
        {
            string path = Path.Combine(directory, name);
            if (!File.Exists(path)) continue;
            try
            {
                var bytes = ReadBounded(path);
                var manifest = Parse(bytes);
                if (selected is null) { selected = manifest; authority = path; selectedBytes = bytes; }
                else if (!selectedBytes!.AsSpan().SequenceEqual(bytes)) diagnostics.Add($"Alias {name} differs; {Path.GetFileName(authority)} is authoritative. Use explicit alias repair.");
            }
            catch (UnsupportedProjectSchemaException) when (selected is not null)
            { diagnostics.Add($"Alias {name} has an unsupported version; the supported authority remains {Path.GetFileName(authority)}."); }
            catch (UnsupportedProjectSchemaException) { throw; }
            catch (Exception error) when (error is IOException or InvalidDataException or JsonException or InvalidOperationException or ArgumentException)
            { diagnostics.Add($"{name}: {error.Message}"); }
        }
        if (selected is null) throw new InvalidDataException("No supported project manifest is readable. " + string.Join(" ", diagnostics));
        if (Path.GetFileName(authority) != "manifest.json") diagnostics.Add("Using session.manifest.json because the authority is absent or invalid; no file was changed.");
        var missing = Assets(selected).Where(asset => !File.Exists(ResolvePath(directory, asset.File))).ToArray();
        diagnostics.AddRange(missing.Select(asset => $"Missing {asset.Role} asset: {asset.File}. Locate the file to relink it; the project is preserved."));
        return new(selected, authority!, diagnostics, missing);
    }

    public static SessionManifest Parse(ReadOnlySpan<byte> bytes)
    {
        if (bytes.Length is <= 0 or > MaximumBytes) throw new InvalidDataException("Project manifest exceeds its byte budget.");
        using var document = JsonDocument.Parse(bytes.ToArray(), new() { AllowTrailingCommas = true, MaxDepth = 64 });
        var root = document.RootElement;
        if (root.ValueKind != JsonValueKind.Object) throw new InvalidDataException("Project manifest must be an object.");
        var fields=new HashSet<string>(StringComparer.OrdinalIgnoreCase);
        if(root.EnumerateObject().Any(property=>!fields.Add(property.Name)))throw new InvalidDataException("Ambiguous duplicate project fields.");
        var versionProperty = root.EnumerateObject().FirstOrDefault(value => value.Name.Equals("Version", StringComparison.OrdinalIgnoreCase));
        if (versionProperty.Value.ValueKind != JsonValueKind.Number || !versionProperty.Value.TryGetInt32(out int version) || version is not (2 or 3))
            throw new UnsupportedProjectSchemaException("Unsupported project schema version. Keep the project and open it with a compatible NessStudio version.");
        var manifest = JsonSerializer.Deserialize<SessionManifest>(bytes, Options) ?? throw new InvalidDataException("Empty project manifest.");
        if (version == 3)
        {
            if (manifest.Schema != Schema || string.IsNullOrWhiteSpace(manifest.ProjectId) || string.IsNullOrWhiteSpace(manifest.SessionId) ||
                string.IsNullOrWhiteSpace(manifest.EngineVersion) || manifest.State is not ("Completed" or "Recovered" or "Interrupted" or "Failed") || manifest.Assets is null || manifest.Assets.Count is < 1 or > 16)
                throw new InvalidDataException("Invalid v3 project identity, state or assets.");
            if (manifest.Assets.Select(asset => asset.Id).Distinct(StringComparer.Ordinal).Count() != manifest.Assets.Count || manifest.Assets.Select(asset => asset.Role).Distinct(StringComparer.Ordinal).Count() != manifest.Assets.Count)
                throw new InvalidDataException("Duplicate project asset identity or role.");
            foreach (var asset in manifest.Assets)
                if (string.IsNullOrWhiteSpace(asset.Id) || string.IsNullOrWhiteSpace(asset.File) || asset.File.IndexOf('\0') >= 0 || asset.DurationHns < 0 ||
                    asset.Role is not ("screen" or "camera" or "program" or "microphone" or "system" or "primary") || asset.Kind is not ("video" or "audio" or "av") || !asset.Finalized)
                    throw new InvalidDataException("Invalid project asset metadata.");
            if(manifest.Assets.Any(asset=>((asset.Role is "screen" or "camera" or "program")&&asset.Kind!="video")||((asset.Role is "microphone" or "system")&&asset.Kind!="audio")))throw new InvalidDataException("Project asset role and media kind disagree.");
            ProjectTracks(manifest);
        }
        return manifest;
    }

    public static IReadOnlyList<ProjectAsset> Assets(SessionManifest manifest)
    {
        if (manifest.Version == 3) return manifest.Assets;
        var result = new List<ProjectAsset>();
        AddVideo(result, "screen", manifest.Screen); AddVideo(result, "camera", manifest.Webcam); AddVideo(result, "program", manifest.Program);
        AddAudio(result, "microphone", manifest.Mic); AddAudio(result, "system", manifest.System);
        return result;
    }
    private static long Duration(string? text) => TimeSpan.TryParse(text, CultureInfo.InvariantCulture, out var value) ? Math.Max(0, value.Ticks) : 0;
    private static void AddVideo(List<ProjectAsset> assets, string role, SessionManifest.VideoTrack? track)
    { if (track is not null && !string.IsNullOrWhiteSpace(track.File)) assets.Add(new() { Id = role, Role = role, File = track.File, Kind = "video", Codec = track.PixelFormat == "nv12" ? "rawvideo" : "h264", Container = track.ContainerKind ?? string.Empty, DurationHns = Duration(track.Duration), Video = track }); }
    private static void AddAudio(List<ProjectAsset> assets, string role, SessionManifest.AudioTrack? track)
    { if (track is not null && !string.IsNullOrWhiteSpace(track.File)) assets.Add(new() { Id = role, Role = role, File = track.File, Kind = "audio", Codec = $"pcm_s{track.BitsPerSample}le", Container = "wav", DurationHns = Duration(track.Duration), Audio = track }); }
    private static void ProjectTracks(SessionManifest manifest)
    {
        SessionManifest.VideoTrack? Video(string role)
        {
            var asset = manifest.Assets.SingleOrDefault(asset => asset.Role == role);
            if (asset is null) return null;
            var track = asset.Video ?? throw new InvalidDataException("Video asset has no format metadata."); track.File = asset.File; return track;
        }
        SessionManifest.AudioTrack? Audio(string role)
        {
            var asset = manifest.Assets.SingleOrDefault(asset => asset.Role == role);
            if (asset is null) return null;
            var track = asset.Audio ?? throw new InvalidDataException("Audio asset has no format metadata."); track.File = asset.File; return track;
        }
        manifest.Screen = Video("screen")!; manifest.Webcam = Video("camera")!; manifest.Program = Video("program");
        manifest.Mic = Audio("microphone")!; manifest.System = Audio("system")!;
        manifest.HasScreen = manifest.Screen is not null; manifest.HasWebcam = manifest.Webcam is not null; manifest.HasProgram = manifest.Program is not null;
        manifest.HasMic = manifest.Mic is not null; manifest.HasSystemAudio = manifest.System is not null;
        manifest.PrimaryAsset = manifest.Assets.SingleOrDefault(asset => asset.Role == "primary")?.File ?? manifest.PrimaryAsset;
    }
    public static string ResolvePath(string directory, string file)
    {
        if (string.IsNullOrWhiteSpace(file) || file.IndexOf('\0') >= 0) throw new InvalidDataException("Missing asset path.");
        return Path.GetFullPath(Path.IsPathRooted(file) ? file : Path.Combine(directory, file));
    }
    public static string? ExistingPath(string directory, string? file) => string.IsNullOrWhiteSpace(file) ? null : File.Exists(ResolvePath(directory, file)) ? ResolvePath(directory, file) : null;
    private static byte[] ReadBounded(string path)
    { using var file = File.OpenRead(path); if (file.Length is <= 0 or > MaximumBytes) throw new InvalidDataException("Manifest byte budget exceeded."); var data = new byte[(int)file.Length]; file.ReadExactly(data); return data; }

    public static string WriteAliases(string directory, SessionManifest manifest, bool overwrite = false)
    {
        Directory.CreateDirectory(directory);
        byte[] bytes = JsonSerializer.SerializeToUtf8Bytes(manifest, Options); _ = Parse(bytes);
        string primary = Path.Combine(directory, "manifest.json"), alias = Path.Combine(directory, "session.manifest.json");
        WriteAtomic(primary, bytes, overwrite); WriteAtomic(alias, bytes, overwrite); return primary;
    }
    public static void WriteAtomic(string path, ReadOnlySpan<byte> bytes, bool overwrite = false)
    {
        string pending = path + "." + Guid.NewGuid().ToString("N") + ".pending";
        using (var file = new FileStream(pending, FileMode.CreateNew, FileAccess.Write, FileShare.None)) { file.Write(bytes); file.Flush(flushToDisk: true); }
        File.Move(pending, path, overwrite);
    }
    public static void UpgradeNewRecording(SessionManifest manifest, string directory, string engineVersion)
    {
        var assets = Assets(manifest).ToList(); manifest.Version = CurrentVersion; manifest.Schema = Schema;
        manifest.ProjectId ??= Guid.NewGuid().ToString("N"); manifest.EngineVersion = engineVersion; manifest.State = "Completed"; manifest.FinalizationResult = "MediaFinalized";
        if (!string.IsNullOrWhiteSpace(manifest.PrimaryAsset) && assets.All(asset => !asset.File.Equals(manifest.PrimaryAsset, StringComparison.OrdinalIgnoreCase)))
            assets.Add(new() { Id = "primary", Role = "primary", File = manifest.PrimaryAsset, Kind = manifest.HasScreen || manifest.HasWebcam || manifest.HasProgram ? "av" : "audio", Codec = manifest.HasScreen || manifest.HasWebcam || manifest.HasProgram ? "h264" : "pcm_s16le", Container = Path.GetExtension(manifest.PrimaryAsset).TrimStart('.'), DurationHns = checked((manifest.DurationMs ?? 0) * TimeSpan.TicksPerMillisecond) });
        foreach (var asset in assets) { string path = ResolvePath(directory, asset.File); if (!File.Exists(path)) throw new FileNotFoundException("Finalized project asset is missing.", path); asset.Bytes = new FileInfo(path).Length; }
        manifest.Assets = assets;
    }
    public static void RepairAliases(string directory)
    { var document = Read(directory); BackupAuthority(directory); WriteAliases(directory, document.Manifest, overwrite: true); }
    private static void BackupAuthority(string directory)
    { foreach(string name in new[]{"manifest.json","session.manifest.json"}){string source=Path.Combine(directory,name);if(File.Exists(source))File.Copy(source,source+"."+Guid.NewGuid().ToString("N")+".before-repair",overwrite:false);} }
    public static void RelocateAsset(string directory, string id, string replacement)
    {
        replacement = Path.GetFullPath(replacement); if (!File.Exists(replacement)) throw new FileNotFoundException("Replacement asset was not found.", replacement);
        var document = Read(directory); var asset = Assets(document.Manifest).SingleOrDefault(asset => asset.Id == id) ?? throw new InvalidDataException("Unknown project asset.");
        string file = Path.GetRelativePath(Path.GetFullPath(directory), replacement);
        if (asset.Bytes is { } bytes && new FileInfo(replacement).Length != bytes) throw new InvalidDataException("The replacement size differs from the recorded asset.");
        if(document.Manifest.PrimaryAsset==asset.File)document.Manifest.PrimaryAsset=file;
        asset.File = file; if (asset.Video is not null) asset.Video.File = file; if (asset.Audio is not null) asset.Audio.File = file;
        if (document.Manifest.Version == 3) ProjectTracks(document.Manifest);
        BackupAuthority(directory); WriteAliases(directory, document.Manifest, overwrite: true);
    }
    public static void RecordRegistration(string directory, string projectId, bool registered, string? error = null)
    { WriteAtomic(Path.Combine(directory, "project-registration.json"), JsonSerializer.SerializeToUtf8Bytes(new { version = 1, projectId, registered, recoverable = !registered, sourceAssetsPreserved = true, error }, Options), overwrite: true); }
}

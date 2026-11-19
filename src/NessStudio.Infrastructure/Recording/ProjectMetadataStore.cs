using System.Security.Cryptography;
using System.Text.Json;
using System.Text.Json.Nodes;

namespace NessStudio.Recording;

/// <summary>Consolidates completed recording control documents without changing media or old projects on read.</summary>
public static class ProjectMetadataStore
{
    private const int MaximumDocumentBytes = 1048576;
    private static readonly HashSet<string> Names = new(StringComparer.Ordinal)
    {
        "screen.mkv.manifest.json", "webcam.mkv.manifest.json", "program.mkv.manifest.json", "audio.native.json",
        "screen.mkv.partial.manifest.json", "webcam.mkv.partial.manifest.json", "program.mkv.partial.manifest.json",
        "recording-native.json", "recording-startup-error.json", "recording-incomplete.json",
        "project-registration.json", "project-recovery.json", "session.manifest.json", "preview.png.thumbnail.json"
    };

    public static void SetDocument(SessionManifest manifest, string name, JsonElement document)
    {
        if (Path.GetFileName(name) != name || document.ValueKind != JsonValueKind.Object)
            throw new InvalidDataException("Invalid embedded recording document.");
        (manifest.Metadata ??= new()).Documents[name] = document.Clone();
    }

    public static JsonDocument? ReadDocument(string directory, string name, SessionManifest? manifest = null)
    {
        if (Path.GetFileName(name) != name) throw new InvalidDataException("A recording document must have a local file name.");
        manifest ??= TryReadProject(directory);
        JsonElement? embedded = name switch
        {
            "project-registration.json" => manifest?.Metadata?.Registration,
            "project-recovery.json" => manifest?.Metadata?.Recovery,
            _ => null
        };
        if (embedded is null && manifest?.Metadata?.Documents.TryGetValue(name, out var value) == true) embedded = value;
        if (embedded is { } element) return JsonDocument.Parse(element.GetRawText(), new() { MaxDepth = 64 });
        string path = Path.Combine(directory, name);
        if (!File.Exists(path)) return null;
        return JsonDocument.Parse(ReadBounded(path), new() { MaxDepth = 64 });
    }

    private static SessionManifest? TryReadProject(string directory)
    {
        if (!File.Exists(Path.Combine(directory, "manifest.json"))) return null;
        try { return ProjectManifestStore.Read(directory).Manifest; }
        catch (Exception error) when (error is IOException or InvalidDataException or JsonException or InvalidOperationException or ArgumentException)
        { return null; }
    }

    private static byte[] ReadBounded(string path)
    {
        using var file = new FileStream(path, FileMode.Open, FileAccess.Read, FileShare.Read);
        if (file.Length is <= 0 or > MaximumDocumentBytes) throw new InvalidDataException("Recording metadata exceeds its document limit.");
        byte[] bytes = new byte[(int)file.Length]; file.ReadExactly(bytes); return bytes;
    }

    private static bool Generated(string name) => Names.Contains(name) ||
        new[] { "screen.mkv.session-", "webcam.mkv.session-", "program.mkv.session-", "audio.native.json.session-", "preview.png.job-" }
            .Any(prefix => name.StartsWith(prefix, StringComparison.Ordinal) && name.EndsWith(".json", StringComparison.Ordinal));

    /// <summary>Consolidates completed metadata without blocking the application's presentation thread.</summary>
    public static Task ConsolidateAsync(string directory, SessionManifest manifest) =>
        Task.Run(() => Consolidate(directory, manifest));

    public static void Consolidate(string directory, SessionManifest manifest)
    {
        directory = Path.GetFullPath(directory);
        manifest.Metadata ??= new();
        if (manifest.Metadata.Version != 1) throw new InvalidDataException("Unsupported embedded recording metadata version.");
        var collected = new List<(string Path, byte[] Bytes, JsonElement Document)>();
        foreach (string path in Directory.EnumerateFiles(directory, "*.json", SearchOption.TopDirectoryOnly))
        {
            string name = Path.GetFileName(path);
            if (!Generated(name)) continue;
            if ((File.GetAttributes(path) & FileAttributes.ReparsePoint) != 0)
                throw new InvalidDataException("Recording metadata cannot be consolidated through a link.");
            byte[] bytes = ReadBounded(path);
            using var document = JsonDocument.Parse(bytes, new() { MaxDepth = 64 });
            if (document.RootElement.ValueKind != JsonValueKind.Object) throw new InvalidDataException("Recording control metadata must be an object.");
            // Active owners retain their journals. A completed capture or an
            // awaited thumbnail job has already released its native resources.
            if (document.RootElement.TryGetProperty("kind", out var kind) && kind.GetString() == "nessstudio.operation-journal" &&
                document.RootElement.GetProperty("snapshot").GetProperty("cleanupPending").GetBoolean())
                throw new InvalidOperationException("Native resources must finish before their metadata is consolidated.");
            var value = document.RootElement.Clone();
            manifest.Metadata.Documents[name] = value;
            if (name == "project-registration.json") manifest.Metadata.Registration = value;
            if (name == "project-recovery.json") manifest.Metadata.Recovery = value;
            collected.Add((path, bytes, value));
        }
        // Validate and durably publish the complete replacement before any
        // original metadata file is retired. Media files are never touched.
        ProjectManifestStore.WriteAliases(directory, manifest, overwrite: File.Exists(Path.Combine(directory, "manifest.json")));
        var persisted = ProjectManifestStore.Read(directory).Manifest.Metadata
            ?? throw new InvalidDataException("The consolidated recording metadata was not saved.");
        foreach (var item in collected)
        {
            string name = Path.GetFileName(item.Path);
            if (!persisted.Documents.TryGetValue(name, out var saved) ||
                !JsonNode.DeepEquals(JsonNode.Parse(item.Document.GetRawText()), JsonNode.Parse(saved.GetRawText())))
                throw new InvalidDataException("Recording metadata verification failed; original documents were preserved.");
        }
        foreach (var item in collected)
        {
            if (!SHA256.HashData(ReadBounded(item.Path)).AsSpan().SequenceEqual(SHA256.HashData(item.Bytes)))
                throw new IOException("Recording metadata changed during consolidation; the original document was preserved.");
            File.Delete(item.Path);
        }
    }
}

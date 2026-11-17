using System.Text.Json;
using System.Text.Json.Serialization;

namespace NessStudio.Recording;

/// <summary>Versioned media identity independent of a conventional filename or capture backend.</summary>
public sealed class ProjectAsset
{
    public string Id { get; set; } = string.Empty;
    public string Role { get; set; } = string.Empty;
    public string File { get; set; } = string.Empty;
    public string Kind { get; set; } = string.Empty;
    public string Codec { get; set; } = string.Empty;
    public string Container { get; set; } = string.Empty;
    public long DurationHns { get; set; }
    public long? Bytes { get; set; }
    public SessionManifest.VideoTrack? Video { get; set; }
    public SessionManifest.AudioTrack? Audio { get; set; }
    public bool Finalized { get; set; } = true;
    [JsonExtensionData] public Dictionary<string, JsonElement>? UnknownFields { get; set; }
}

/// <summary>Read-only resolution result. Opening a project never repairs or rewrites its aliases.</summary>
public sealed record ProjectManifestDocument(SessionManifest Manifest, string AuthorityPath,
    IReadOnlyList<string> Diagnostics, IReadOnlyList<ProjectAsset> MissingAssets);

/// <summary>Reports an unsupported schema without interpreting stale aliases as current media.</summary>
public sealed class UnsupportedProjectSchemaException(string message) : IOException(message);

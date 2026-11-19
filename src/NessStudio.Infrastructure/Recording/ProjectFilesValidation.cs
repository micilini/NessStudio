using System.Buffers.Binary;
using System.Text.Json;

namespace NessStudio.Recording;

/// <summary>Describes whether a library action can use its recorded project without changing any files.</summary>
public sealed record ProjectFilesValidation(bool IsValid, bool RemoveFromLibrary, string Message)
{
    /// <summary>Checks the current folder, supported metadata and all retained capture assets on demand.</summary>
    public static ProjectFilesValidation Check(string? directory)
    {
        try
        {
            if (string.IsNullOrWhiteSpace(directory))
                return Missing("The recording folder was not found.");
            // Directory.Exists reports permission and device failures as false.
            // Keep those distinct from confirmed missing files before pruning.
            if ((File.GetAttributes(directory) & FileAttributes.Directory) == 0)
                return Missing("The recording folder was not found.");
            foreach (string name in new[] { "manifest.json", "session.manifest.json" })
            {
                try
                {
                    string path = Path.Combine(directory, name);
                    if ((File.GetAttributes(path) & FileAttributes.Directory) != 0)
                        throw new InvalidDataException("A project manifest is not a file.");
                    using var metadata = File.OpenRead(path);
                    _ = metadata.ReadByte();
                }
                catch (FileNotFoundException) { }
                catch (DirectoryNotFoundException) { }
            }

            var document = ProjectManifestStore.Read(directory);
            var manifest = document.Manifest;
            if (manifest.Version == 2 && (manifest.HasScreen && string.IsNullOrWhiteSpace(manifest.Screen?.File) ||
                manifest.HasWebcam && string.IsNullOrWhiteSpace(manifest.Webcam?.File) ||
                manifest.HasProgram && string.IsNullOrWhiteSpace(manifest.Program?.File) ||
                manifest.HasMic && string.IsNullOrWhiteSpace(manifest.Mic?.File) ||
                manifest.HasSystemAudio && string.IsNullOrWhiteSpace(manifest.System?.File)))
                return Missing("The recording metadata is incomplete.");
            var assets = ProjectManifestStore.Assets(manifest).ToList();
            if (assets.Count == 0)
                return Missing("The project has no recorded media files.");

            // Earlier manifests can name a finalized primary file separately from
            // their source tracks. It is also required when explicitly retained.
            if (!string.IsNullOrWhiteSpace(manifest.PrimaryAsset) &&
                assets.All(asset => !ProjectManifestStore.ResolvePath(directory, asset.File).Equals(
                    ProjectManifestStore.ResolvePath(directory, manifest.PrimaryAsset), StringComparison.OrdinalIgnoreCase)))
                assets.Add(new ProjectAsset { File = manifest.PrimaryAsset });

            foreach (var asset in assets)
            {
                string path = ProjectManifestStore.ResolvePath(directory, asset.File);
                using var file = new FileStream(path, FileMode.Open, FileAccess.Read,
                    FileShare.ReadWrite | FileShare.Delete);
                if (file.Length <= 0 || asset.Bytes is { } bytes && file.Length != bytes)
                    return Missing("A recording file is empty, changed or incomplete: " + asset.File);
                if (!HasExpectedContainer(file, asset))
                    return Missing("A recording file has an invalid media header: " + asset.File);
                // Probe both ends without reading a potentially very large video
                // into memory or starting a media process for every library click.
                if (file.ReadByte() < 0 || file.Seek(-1, SeekOrigin.End) < 0 || file.ReadByte() < 0)
                    return Missing("A recording file is incomplete: " + asset.File);
            }

            return new(true, false, string.Empty);
        }
        catch (UnsupportedProjectSchemaException error)
        {
            // A project from a newer app is not evidence that its media is lost.
            return new(false, false, error.Message);
        }
        catch (Exception error) when (error is FileNotFoundException or DirectoryNotFoundException)
        {
            return Missing("One or more recording files were not found.");
        }
        catch (Exception error) when (error is InvalidDataException or JsonException or ArgumentException or NotSupportedException)
        {
            return Missing("The recording metadata or media files are missing or damaged.");
        }
        catch (Exception error) when (error is IOException or UnauthorizedAccessException or System.Security.SecurityException)
        {
            // A disconnected device, sharing violation or permission failure can
            // be temporary. Keep the library entry so the user can retry.
            return new(false, false, "The recording files could not be accessed. Check the drive and file permissions, then try again.");
        }
    }

    private static bool HasExpectedContainer(FileStream file, ProjectAsset asset)
    {
        string container = (string.IsNullOrWhiteSpace(asset.Container) ? asset.Video?.ContainerKind : asset.Container)?
            .Trim().ToLowerInvariant() ?? string.Empty;
        // Older raw intermediates have no container signature to inspect.
        if (asset.Video?.IsRawIntermediate == true || container is "raw" or "rawvideo" or "nv12") return true;
        string extension = Path.GetExtension(asset.File).ToLowerInvariant();
        bool matroska = container is "matroska" or "mkv" or "webm" || extension is ".mkv" or ".webm";
        bool wave = container is "wav" or "wave" or "rf64" || extension is ".wav" or ".wave";
        bool mp4 = container is "mp4" or "mov" or "isobmff" || extension is ".mp4" or ".m4v" or ".mov";
        file.Position = 0;
        Span<byte> header = stackalloc byte[12];
        if (matroska)
            return ReadHeader(file, header[..4]) && header[..4].SequenceEqual(new byte[] { 0x1A, 0x45, 0xDF, 0xA3 });
        if (wave)
            return ReadHeader(file, header) && (header[..4].SequenceEqual("RIFF"u8) || header[..4].SequenceEqual("RF64"u8)) &&
                header[8..12].SequenceEqual("WAVE"u8);
        if (mp4) return HasMp4FileType(file);
        // Preserve unfamiliar legacy types rather than claiming a format we
        // cannot validate. Their recorded lengths and readability still apply.
        return true;
    }

    private static bool HasMp4FileType(FileStream file)
    {
        Span<byte> header = stackalloc byte[16];
        long position = 0;
        // Seek over optional leading boxes without reading their payloads. At
        // most 32 headers (512 bytes) are inspected, including large free boxes.
        for (int count = 0; count < 32 && file.Length - position >= 8; count++)
        {
            file.Position = position;
            if (!ReadHeader(file, header[..8])) return false;
            ulong size = BinaryPrimitives.ReadUInt32BigEndian(header[..4]);
            int headerBytes = 8;
            if (size == 1)
            {
                if (!ReadHeader(file, header[8..16])) return false;
                size = BinaryPrimitives.ReadUInt64BigEndian(header[8..16]);
                headerBytes = 16;
            }
            else if (size == 0) size = (ulong)(file.Length - position);
            if (size < (ulong)headerBytes || size > (ulong)(file.Length - position)) return false;
            if (header[4..8].SequenceEqual("ftyp"u8))
                return size >= (ulong)(headerBytes + 8) && ReadHeader(file, header[..8]);
            position += (long)size;
        }
        return false;
    }

    private static bool ReadHeader(FileStream file, Span<byte> bytes)
    {
        int offset = 0;
        while (offset < bytes.Length)
        {
            int read = file.Read(bytes[offset..]);
            if (read == 0) return false;
            offset += read;
        }
        return true;
    }

    private static ProjectFilesValidation Missing(string reason) => new(false, true,
        reason + "\n\nClick OK to remove this recording from the library.");
}

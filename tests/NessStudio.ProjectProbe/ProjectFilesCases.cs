using System.Buffers.Binary;
using NessStudio.Recording;

internal static partial class Program
{
    private static SessionManifest LibraryManifest(string folder, bool legacy = false)
    {
        var manifest = V2(folder);
        var bytes = new byte[128];
        // A Matroska EBML header and segment marker; these fixtures exercise
        // bounded container checks rather than claiming decodable video frames.
        Convert.FromHexString("1A45DFA3A34286810142F7810142F2810442F381084282886D6174726F736B61428781044285810218538067FF").CopyTo(bytes, 0);
        File.WriteAllBytes(Path.Combine(folder, manifest.Screen.File), bytes);
        if (!legacy) ProjectManifestStore.UpgradeNewRecording(manifest, folder, "libnessstudio/test-only");
        return manifest;
    }

    private static byte[] WaveHeader(bool rf64)
    {
        var bytes = new byte[128];
        (rf64 ? "RF64"u8 : "RIFF"u8).CopyTo(bytes);
        BinaryPrimitives.WriteUInt32LittleEndian(bytes.AsSpan(4), rf64 ? uint.MaxValue : 120);
        "WAVE"u8.CopyTo(bytes.AsSpan(8));
        int format = rf64 ? 48 : 12;
        if (rf64)
        {
            "ds64"u8.CopyTo(bytes.AsSpan(12));
            BinaryPrimitives.WriteUInt32LittleEndian(bytes.AsSpan(16), 28);
            BinaryPrimitives.WriteUInt64LittleEndian(bytes.AsSpan(20), 120);
            BinaryPrimitives.WriteUInt64LittleEndian(bytes.AsSpan(28), 48);
            BinaryPrimitives.WriteUInt64LittleEndian(bytes.AsSpan(36), 12);
        }
        "fmt "u8.CopyTo(bytes.AsSpan(format));
        BinaryPrimitives.WriteUInt32LittleEndian(bytes.AsSpan(format + 4), 16);
        BinaryPrimitives.WriteUInt16LittleEndian(bytes.AsSpan(format + 8), 1);
        BinaryPrimitives.WriteUInt16LittleEndian(bytes.AsSpan(format + 10), 2);
        BinaryPrimitives.WriteUInt32LittleEndian(bytes.AsSpan(format + 12), 48000);
        BinaryPrimitives.WriteUInt32LittleEndian(bytes.AsSpan(format + 16), 192000);
        BinaryPrimitives.WriteUInt16LittleEndian(bytes.AsSpan(format + 20), 4);
        BinaryPrimitives.WriteUInt16LittleEndian(bytes.AsSpan(format + 22), 16);
        "data"u8.CopyTo(bytes.AsSpan(format + 24));
        BinaryPrimitives.WriteUInt32LittleEndian(bytes.AsSpan(format + 28), rf64 ? uint.MaxValue : 84);
        return bytes;
    }

    private static void ProjectFilesCases()
    {
        Case("library-action-missing-folder-and-manifest-rejected", () =>
        {
            Check(ProjectFilesValidation.Check(Path.Combine(_root, "never recorded")).RemoveFromLibrary,
                "A missing project folder was accepted.");
            Check(ProjectFilesValidation.Check(Folder("missing manifest")).RemoveFromLibrary,
                "A project without metadata was accepted.");
        });
        Case("library-action-all-source-tracks-required", () =>
        {
            string folder = Folder("missing secondary source");
            var manifest = LibraryManifest(folder);
            manifest.Assets.Add(new() { Id = "microphone", Role = "microphone", Kind = "audio", File = "mic.wav",
                Audio = new() { File = "mic.wav", SampleRate = 48000, Channels = 1, BitsPerSample = 16 } });
            ProjectManifestStore.WriteAliases(folder, manifest);
            Check(ProjectFilesValidation.Check(folder).RemoveFromLibrary,
                "The intact video hid a missing retained microphone file.");
            Check(File.Exists(Path.Combine(folder, manifest.Screen.File)), "Validation changed the surviving source.");
        });
        Case("library-action-changed-or-empty-finalized-files-rejected", () =>
        {
            string folder = Folder("truncated source");
            var manifest = LibraryManifest(folder);
            ProjectManifestStore.WriteAliases(folder, manifest);
            string path = Path.Combine(folder, manifest.Screen.File);
            File.WriteAllBytes(path, new byte[127]);
            Check(ProjectFilesValidation.Check(folder).RemoveFromLibrary, "A changed recorded byte length was accepted.");
            File.WriteAllBytes(path, []);
            Check(ProjectFilesValidation.Check(folder).RemoveFromLibrary, "An empty source file was accepted.");
        });
        Case("library-action-valid-v3-read-only-without-optional-thumbnail", () =>
        {
            string folder = Folder("valid project action");
            var manifest = LibraryManifest(folder);
            manifest.Thumbnail = "missing optional preview.png";
            ProjectManifestStore.WriteAliases(folder, manifest);
            string authority = Path.Combine(folder, "manifest.json");
            string before = Hash(authority);
            var check = ProjectFilesValidation.Check(folder);
            Check(check.IsValid && !check.RemoveFromLibrary && Hash(authority) == before,
                "An intact project was rejected or metadata was rewritten.");
        });
        Case("library-action-legacy-alias-and-moved-folder-supported", () =>
        {
            string folder = Folder("legacy library action");
            var manifest = LibraryManifest(folder, legacy: true);
            File.WriteAllBytes(Path.Combine(folder, "session.manifest.json"), Json(manifest));
            string moved = Path.Combine(_root, "relocated legacy library action");
            Directory.Move(folder, moved);
            Check(ProjectFilesValidation.Check(moved).IsValid && !File.Exists(Path.Combine(moved, "manifest.json")),
                "A legacy alias or relative project move was rejected or rewritten.");
        });
        Case("library-action-separate-legacy-primary-file-required", () =>
        {
            string folder = Folder("legacy primary asset");
            var manifest = LibraryManifest(folder, legacy: true);
            manifest.PrimaryAsset = "finalized.mp4";
            ProjectManifestStore.WriteAliases(folder, manifest);
            Check(ProjectFilesValidation.Check(folder).RemoveFromLibrary,
                "A missing explicitly retained primary asset was ignored.");
        });
        Case("library-action-unsupported-schema-and-locked-file-preserved", () =>
        {
            string folder = Folder("newer library project");
            File.WriteAllText(Path.Combine(folder, "manifest.json"), "{\"Version\":99}");
            var unsupported = ProjectFilesValidation.Check(folder);
            Check(!unsupported.IsValid && !unsupported.RemoveFromLibrary, "A newer project was treated as missing media.");

            string lockedFolder = Folder("locked library project");
            var manifest = LibraryManifest(lockedFolder);
            ProjectManifestStore.WriteAliases(lockedFolder, manifest);
            using var locked = new FileStream(Path.Combine(lockedFolder, manifest.Screen.File), FileMode.Open,
                FileAccess.Read, FileShare.None);
            var denied = ProjectFilesValidation.Check(lockedFolder);
            Check(!denied.IsValid && !denied.RemoveFromLibrary, "A temporarily locked file would remove a valid project.");
        });
        Case("library-action-same-size-corrupt-matroska-rejected", () =>
        {
            string folder = Folder("same size corrupt media");
            var manifest = LibraryManifest(folder);
            ProjectManifestStore.WriteAliases(folder, manifest);
            string path = Path.Combine(folder, manifest.Screen.File);
            File.WriteAllBytes(path, new byte[128]);
            Check(ProjectFilesValidation.Check(folder).RemoveFromLibrary,
                "An arbitrary replacement with the recorded byte length passed as Matroska.");
        });
        Case("library-action-riff-and-rf64-wave-signatures", () =>
        {
            foreach (bool rf64 in new[] { false, true })
            {
                string folder = Folder("wave signature " + rf64);
                var bytes = WaveHeader(rf64);
                string path = Path.Combine(folder, "mic.wav");
                File.WriteAllBytes(path, bytes);
                var manifest = new SessionManifest { Version = 2, SessionId = "audio-only", BaseDir = folder,
                    HasMic = true, Mic = new() { File = "mic.wav", SampleRate = 48000, Channels = 2, BitsPerSample = 16 } };
                ProjectManifestStore.UpgradeNewRecording(manifest, folder, "libnessstudio/test-only");
                ProjectManifestStore.WriteAliases(folder, manifest);
                Check(ProjectFilesValidation.Check(folder).IsValid, "A supported WAVE header was rejected.");
                bytes[8] = 0;
                File.WriteAllBytes(path, bytes);
                Check(ProjectFilesValidation.Check(folder).RemoveFromLibrary,
                    "An invalid WAVE signature with unchanged size was accepted.");
            }
        });
        Case("library-action-legacy-mp4-ftyp-after-free-box", () =>
        {
            string folder = Folder("legacy mp4 signature");
            var manifest = LibraryManifest(folder, legacy: true);
            manifest.PrimaryAsset = "primary.mp4";
            var bytes = new byte[128];
            // Extended-size free box followed by the file type box is allowed.
            BinaryPrimitives.WriteUInt32BigEndian(bytes.AsSpan(0), 1);
            "free"u8.CopyTo(bytes.AsSpan(4));
            BinaryPrimitives.WriteUInt64BigEndian(bytes.AsSpan(8), 16);
            BinaryPrimitives.WriteUInt32BigEndian(bytes.AsSpan(16), 24);
            "ftypisom"u8.CopyTo(bytes.AsSpan(20));
            "isomiso2"u8.CopyTo(bytes.AsSpan(32));
            BinaryPrimitives.WriteUInt32BigEndian(bytes.AsSpan(40), 88);
            "free"u8.CopyTo(bytes.AsSpan(44));
            string path = Path.Combine(folder, manifest.PrimaryAsset);
            File.WriteAllBytes(path, bytes);
            ProjectManifestStore.WriteAliases(folder, manifest);
            Check(ProjectFilesValidation.Check(folder).IsValid, "A supported legacy MP4 file type box was rejected.");
            bytes[20] = 0;
            File.WriteAllBytes(path, bytes);
            Check(ProjectFilesValidation.Check(folder).RemoveFromLibrary, "An MP4 without a file type box was accepted.");
        });
        Case("library-action-unknown-legacy-and-explicit-raw-preserved", () =>
        {
            string folder = Folder("legacy unknown container");
            var manifest = LibraryManifest(folder, legacy: true);
            manifest.Screen.ContainerKind = "legacy-vendor";
            manifest.Screen.File = "screen.vendor";
            manifest.PrimaryAsset = manifest.Screen.File;
            File.WriteAllBytes(Path.Combine(folder, manifest.Screen.File), new byte[128]);
            ProjectManifestStore.WriteAliases(folder, manifest);
            Check(ProjectFilesValidation.Check(folder).IsValid, "An unknown legacy container was declared corrupt by guessing its format.");
            manifest.Screen.File = "raw.mkv";
            manifest.PrimaryAsset = manifest.Screen.File;
            manifest.Screen.IsRawIntermediate = true;
            manifest.Screen.ContainerKind = "rawvideo";
            File.WriteAllBytes(Path.Combine(folder, manifest.Screen.File), new byte[128]);
            ProjectManifestStore.WriteAliases(folder, manifest, overwrite: true);
            Check(ProjectFilesValidation.Check(folder).IsValid, "A declared raw intermediate was expected to contain an EBML header.");
        });
    }
}

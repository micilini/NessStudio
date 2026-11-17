using System.Diagnostics;
using System.Text.Json;
using NessStudio.ViewModel.Helpers;
using SQLite;

internal static partial class Program
{
    private sealed class RuntimeDatabaseRow
    {
        [PrimaryKey]
        public int Id { get; set; }
        public string Title { get; set; } = string.Empty;
        public DateTime RecordedAt { get; set; }
        public byte[] Payload { get; set; } = [];
    }

    private static void DatabaseRuntime()
    {
        string folder = Folder("managed database runtime");
        string path = Path.Combine(folder, "owned-runtime.sqlite");
        const string key = "Isolated runtime fixture 'ç音声'";
        DateTime timestamp = new(2026, 11, 14, 12, 34, 56, DateTimeKind.Utc);
        timestamp = timestamp.AddTicks(789);
        byte[] payload = [0, 1, 127, 128, 254, 255];
        DatabaseConnectionManager.Configure(path, key);
        try
        {
            SQLiteConnection connection = DatabaseConnectionManager.GetConnection();
            Case("managed-provider-loads-exact-pinned-native-runtime", () =>
            {
                using JsonDocument manifest = JsonDocument.Parse(File.ReadAllBytes(
                    Path.Combine(AppContext.BaseDirectory, "sqlcipher-runtime.json")));
                JsonElement sources = manifest.RootElement.GetProperty("sources");
                string cipher = connection.ExecuteScalar<string>("PRAGMA cipher_version");
                string sqlite = connection.ExecuteScalar<string>("SELECT sqlite_version()");
                string provider = connection.ExecuteScalar<string>("PRAGMA cipher_provider");
                string providerVersion = connection.ExecuteScalar<string>("PRAGMA cipher_provider_version");
                Check(cipher == sources.GetProperty("sqlcipherVersion").GetString() + " community", "SQLCipher runtime version differs");
                Check(sqlite == sources.GetProperty("sqliteVersion").GetString(), "SQLite runtime version differs");
                Check(provider.Equals("openssl", StringComparison.OrdinalIgnoreCase), "Unexpected SQLCipher crypto provider");
                Check(providerVersion.StartsWith("OpenSSL " + sources.GetProperty("opensslVersion").GetString() + " ", StringComparison.Ordinal), "OpenSSL runtime version differs");
                using Process process = Process.GetCurrentProcess();
                ProcessModule module = process.Modules.Cast<ProcessModule>().Single(m =>
                    Path.GetFileName(m.FileName).Equals("e_sqlcipher.dll", StringComparison.OrdinalIgnoreCase));
                string expectedPath = Path.GetFullPath(Path.Combine(AppContext.BaseDirectory, "e_sqlcipher.dll"));
                Check(Path.GetFullPath(module.FileName).Equals(expectedPath, StringComparison.OrdinalIgnoreCase), "An unpinned SQLCipher DLL was loaded");
                string actualHash = Hash(module.FileName).ToLowerInvariant();
                string? expectedHash = manifest.RootElement.GetProperty("files").EnumerateArray()
                    .Single(f => f.GetProperty("path").GetString() == "e_sqlcipher.dll").GetProperty("sha256").GetString();
                Check(actualHash == expectedHash, "Loaded native runtime bytes differ from the build manifest");
                File.WriteAllText(Path.Combine(folder, "loaded-runtime.json"), JsonSerializer.Serialize(
                    new { status = "PASS", cipher, sqlite, provider, providerVersion, path = module.FileName, sha256 = actualHash,
                        personalDataAccessed = false, formatMigrationRequested = false }));
            });
            Case("managed-encrypted-unicode-blob-and-date-roundtrip", () =>
            {
                connection.CreateTable<RuntimeDatabaseRow>();
                connection.Insert(new RuntimeDatabaseRow { Id = 1, Title = "Projeto 'ç音声' 🎬", RecordedAt = timestamp, Payload = payload });
                DatabaseConnectionManager.CloseConnection();
                DatabaseConnectionManager.Configure(path, key);
                connection = DatabaseConnectionManager.GetConnection();
                RuntimeDatabaseRow row = connection.Find<RuntimeDatabaseRow>(1);
                Check(row.Title == "Projeto 'ç音声' 🎬" && row.RecordedAt.Ticks == timestamp.Ticks && row.Payload.SequenceEqual(payload),
                    "Managed encrypted data changed after reopening");
                row.Title = "Atualizado";
                connection.Update(row);
                Check(connection.Find<RuntimeDatabaseRow>(1).Title == row.Title, "Managed update was lost");
                Check(connection.ExecuteScalar<string>("PRAGMA integrity_check") == "ok", "Managed database failed integrity check");
            });
        }
        finally
        {
            DatabaseConnectionManager.CloseConnection();
        }
        Case("managed-wrong-key-rejected-without-changing-database", () =>
        {
            string before = Hash(path);
            Check(!File.ReadAllBytes(path).AsSpan(0, 16).SequenceEqual("SQLite format 3\0"u8), "The managed fixture was not encrypted");
            Reject<SQLiteException>(() =>
            {
                using var wrong = new SQLiteConnection(new SQLiteConnectionString(path, true, key + "wrong"));
                wrong.ExecuteScalar<int>("SELECT count(*) FROM RuntimeDatabaseRow");
            });
            Check(Hash(path) == before, "Failed managed open changed the encrypted database");
        });
    }
}

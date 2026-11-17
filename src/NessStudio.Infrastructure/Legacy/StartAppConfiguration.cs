using NessStudio.Models;
using SQLite;

namespace NessStudio.ViewModel.Helpers;

/// <summary>Defines the legacy storage paths supplied by the application composition root.</summary>
public sealed record ApplicationStorageOptions(string DataDirectory, string DatabaseFileName = "nesstudio.dll",
    string KeyFileName = "dt-app.nss");

/// <summary>Returns loaded application data without displaying messages or closing the frontend.</summary>
public sealed record StartupConfiguration(SQLiteConnection Connection, SettingsModel Settings,
    RecordingPreferencesModel RecordingPreferences, bool DatabaseAlreadyExisted);

/// <summary>Loads the existing SQLCipher database and preferences without referencing an application singleton.</summary>
public sealed class StartAppConfiguration(ApplicationStorageOptions storage, RecordingPreferencesService preferences)
{
    public StartupConfiguration Initialize()
    {
        Directory.CreateDirectory(storage.DataDirectory);
        string keyPath = Path.Combine(storage.DataDirectory, storage.KeyFileName);
        string dbPath = Path.Combine(storage.DataDirectory, storage.DatabaseFileName);
        if (File.Exists(dbPath) && !File.Exists(keyPath))
            throw new FileNotFoundException("The key for the existing database is missing; the database was preserved.", keyPath);
        if (!File.Exists(keyPath))
            File.WriteAllText(keyPath, DateTimeOffset.UtcNow.ToUnixTimeSeconds().ToString());

        // Existing bytes and key algorithm are deliberately preserved.
        string key = File.ReadAllText(keyPath);
        bool existed = File.Exists(dbPath);
        DatabaseConnectionManager.Configure(dbPath, key);
        var connection = DatabaseConnectionManager.GetConnection();
        if (!existed)
        {
            connection.CreateTable<AppVersion>();
            connection.Insert(new AppVersion());
            connection.CreateTable<SettingsModel>();
            connection.Insert(new SettingsModel());
            connection.CreateTable<ProjectsModel>();
        }
        else
        {
            var columns = connection.Query<TableColumnInfo>("PRAGMA table_info(Settings)");
            if (!columns.Any(column => string.Equals(column.name, "EnableDebugLogs", StringComparison.OrdinalIgnoreCase)))
                connection.Execute("ALTER TABLE Settings ADD COLUMN EnableDebugLogs INTEGER NOT NULL DEFAULT 0");
        }
        var settings = connection.Query<SettingsModel>("SELECT * FROM Settings WHERE Id = ?", 1).FirstOrDefault()
            ?? throw new InvalidDataException("App settings not found in the existing database.");
        return new StartupConfiguration(connection, settings, preferences.Load(), existed);
    }

    private sealed class TableColumnInfo
    {
        public string name { get; set; } = string.Empty;
    }
}

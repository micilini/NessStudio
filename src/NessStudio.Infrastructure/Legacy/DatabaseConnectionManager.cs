using SQLite;

namespace NessStudio.ViewModel.Helpers;

/// <summary>Owns the legacy SQLCipher connection using explicitly supplied storage settings.</summary>
public static class DatabaseConnectionManager
{
    private static readonly object Sync = new();
    private static SQLiteConnection? _connection;
    private static string? _databasePath;
    private static string? _encryptionKey;

    /// <summary>Configures the existing file and key without migrating either.</summary>
    /// <remarks>The key is held only in memory and is never written to diagnostics.</remarks>
    public static void Configure(string databasePath, string encryptionKey)
    {
        ArgumentException.ThrowIfNullOrWhiteSpace(databasePath);
        ArgumentNullException.ThrowIfNull(encryptionKey);
        lock (Sync)
        {
            if (_databasePath == databasePath && _encryptionKey == encryptionKey) return;
            _connection?.Dispose();
            _connection = null;
            _databasePath = databasePath;
            _encryptionKey = encryptionKey;
        }
    }
    public static SQLiteConnection GetConnection()
    {
        lock (Sync)
        {
            if (_databasePath is null || _encryptionKey is null)
                throw new InvalidOperationException("Database storage must be configured before opening it.");
            // Preserve the provider, date representation and SQLCipher key semantics.
            return _connection ??= new SQLiteConnection(new SQLiteConnectionString(_databasePath, true, _encryptionKey));
        }
    }
    public static void CloseConnection()
    {
        lock (Sync)
        {
            _connection?.Dispose();
            _connection = null;
            _encryptionKey = null;
            _databasePath = null;
        }
    }
}

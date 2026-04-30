using NessStudio.Models;
using SQLite;
using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Text;
using System.Threading.Tasks;
using System.Windows;
namespace NessStudio.ViewModel.Helpers
{
    internal class TableColumnInfo
    {
        public int cid { get; set; }
        public string name { get; set; }
        public string type { get; set; }
        public int notnull { get; set; }
        public string dflt_value { get; set; }
        public int pk { get; set; }
    }
    public class StartAppConfiguration
    {
        private string _keyFilePath = Path.Combine(
            Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData),
            "NessStudio",
            "dt-app.nss"
        );
        private string _dbFilePath = Path.Combine(
           Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData),
           "NessStudio",
           ((App)Application.Current).DatabaseFileName
        );
        public bool CheckAndCreateDatabase()
        {
            if (!File.Exists(_keyFilePath))
            {
                var timestamp = DateTimeOffset.UtcNow.ToUnixTimeSeconds().ToString();
                File.WriteAllText(_keyFilePath, timestamp);
            }
            if (!File.Exists(_dbFilePath))
            {
                ((App)Application.Current).KeyDatabase = GetEncryptionKey();
                StartDBSingleton();
                CreateDatabaseAndTables();
                GetAppConfigurationSettings();
                LoadRecordingPreferences();
                return false;
            }
            ((App)Application.Current).KeyDatabase = GetEncryptionKey();
            StartDBSingleton();
            EnsureSettingsSchema();
            GetAppConfigurationSettings();
            LoadRecordingPreferences();
            return true;
        }
        private string GetEncryptionKey()
        {
            if (File.Exists(_keyFilePath))
            {
                return File.ReadAllText(_keyFilePath);
            }
            Application.Current.Dispatcher.Invoke(() =>
            {
                MessageBox.Show("Settings file not found",
                        "Error", MessageBoxButton.OK, MessageBoxImage.Error);
                Application.Current.Shutdown();
            });
            return string.Empty;
        }
        private void StartDBSingleton()
        {
            ((App)Application.Current).DBConnection = DatabaseConnectionManager.GetConnection();
        }
        private void EnsureSettingsSchema()
        {
            try
            {
                var conn = ((App)Application.Current).DBConnection;
                var columns = conn.Query<TableColumnInfo>("PRAGMA table_info(Settings)");
                bool hasEnableDebugLogs = columns.Any(c =>
                    string.Equals(c.name, "EnableDebugLogs", StringComparison.OrdinalIgnoreCase));
                if (!hasEnableDebugLogs)
                {
                    conn.Execute("ALTER TABLE Settings ADD COLUMN EnableDebugLogs INTEGER NOT NULL DEFAULT 0");
                }
            }
            catch
            {
            }
        }
        private void CreateDatabaseAndTables()
        {
            var encryptionKey = GetEncryptionKey();
            var connectionString = new SQLiteConnectionString(_dbFilePath, true, encryptionKey);
            using (var connection = new SQLiteConnection(connectionString))
            {
                connection.CreateTable<AppVersion>();
                InsertAppVersion(connection);
                connection.CreateTable<SettingsModel>();
                InsertDefaultSystemSettings(connection);
                connection.CreateTable<ProjectsModel>();
            }
        }
        private void InsertAppVersion(SQLiteConnection connection)
        {
            var newVersion = new AppVersion();
            connection.Insert(newVersion);
        }
        private void InsertDefaultSystemSettings(SQLiteConnection connection)
        {
            var newSettings = new SettingsModel();
            connection.Insert(newSettings);
        }
        private void GetAppConfigurationSettings()
        {
            try
            {
                var query = "SELECT * FROM Settings WHERE Id = ?";
                var settings = DatabaseHelper.QuerySingle<SettingsModel>(query, 1);
                if (settings != null)
                {
                    ((App)Application.Current).ApplicationIdentifier = settings.ApplicationIdentifier;
                    ((App)Application.Current).AppLanguage = settings.Language;
                    ((App)Application.Current).DebugLogsEnabled = settings.EnableDebugLogs;
                }
                else
                {
                    Application.Current.Dispatcher.Invoke(() =>
                    {
                        MessageBox.Show("App settings not found!",
                        "Error", MessageBoxButton.OK, MessageBoxImage.Error);
                        Application.Current.Shutdown();
                    });
                }
            }
            catch (Exception ex)
            {
                Application.Current.Dispatcher.Invoke(() =>
                {
                    MessageBox.Show("App settings not found!",
                        "Error", MessageBoxButton.OK, MessageBoxImage.Error);
                    Application.Current.Shutdown();
                });
            }
        }
        private void LoadRecordingPreferences()
        {
            try
            {
                var service = new RecordingPreferencesService();
                service.EnsurePreferencesFileExists();
                var prefs = service.Load();
                ((App)Application.Current).RecordingTimerSeconds = prefs.TimerSeconds;
                ((App)Application.Current).RecordingFps = prefs.RecordingFps;
            }
            catch (Exception ex)
            {
                try
                {
                    var service = new RecordingPreferencesService();
                    var fallback = service.ResetToDefault();
                    ((App)Application.Current).RecordingTimerSeconds = fallback.TimerSeconds;
                    ((App)Application.Current).RecordingFps = fallback.RecordingFps;
                }
                catch
                {
                    Application.Current.Dispatcher.Invoke(() =>
                    {
                        MessageBox.Show(
                            "Recording preferences could not be loaded.",
                            "Error",
                            MessageBoxButton.OK,
                            MessageBoxImage.Error
                        );
                        Application.Current.Shutdown();
                    });
                }
            }
        }
    }
}
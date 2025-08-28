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
                return false;
            }

            ((App)Application.Current).KeyDatabase = GetEncryptionKey();
            StartDBSingleton();
            GetAppConfigurationSettings();

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
    }
}

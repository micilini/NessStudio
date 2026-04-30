using System;
using System.Collections.Generic;
using System.Linq;
using System.Text;
using System.Threading.Tasks;
using System.Windows;
using System.Windows.Controls;
using System.Windows.Data;
using System.Windows.Documents;
using System.Windows.Input;
using System.Windows.Media;
using System.Windows.Media.Imaging;
using System.Windows.Shapes;
using NessStudio.Models;
using NessStudio.ViewModel.Helpers;
namespace NessStudio.View.AboutScreen
{
    public partial class AboutScreenWindow : Window
    {
        public AboutScreenWindow()
        {
            InitializeComponent();
            AppId.Text = ((App)Application.Current).ApplicationIdentifier;
            EnableDebugLogsCheckBox.IsChecked = ((App)Application.Current).DebugLogsEnabled;
        }
        public async void CheckForUpdates()
        {
            string url = "https://micilini.com/apps/ness-studio";
            try
            {
                var psi = new System.Diagnostics.ProcessStartInfo
                {
                    FileName = url,
                    UseShellExecute = true
                };
                System.Diagnostics.Process.Start(psi);
            }
            catch (Exception ex)
            {
                MessageBox.Show($"Unable to open the browser. Error: {ex.Message}",
                                "Error", MessageBoxButton.OK, MessageBoxImage.Error);
            }
        }
        private void EnableDebugLogsCheckBox_Checked(object sender, RoutedEventArgs e)
        {
            SetDebugLogsEnabled(true);
        }
        private void EnableDebugLogsCheckBox_Unchecked(object sender, RoutedEventArgs e)
        {
            SetDebugLogsEnabled(false);
        }
        private void SetDebugLogsEnabled(bool enabled)
        {
            try
            {
                var app = (App)Application.Current;
                var settings = DatabaseHelper.QuerySingle<SettingsModel>("SELECT * FROM Settings WHERE Id = ?", 1);
                if (settings == null)
                {
                    MessageBox.Show("App settings not found.",
                                    "Error",
                                    MessageBoxButton.OK,
                                    MessageBoxImage.Error);
                    return;
                }
                settings.EnableDebugLogs = enabled;
                settings.UpdatedAt = DateTime.UtcNow;
                if (!DatabaseHelper.Update(settings))
                {
                    MessageBox.Show("Unable to update debug log settings.",
                                    "Error",
                                    MessageBoxButton.OK,
                                    MessageBoxImage.Error);
                    return;
                }
                app.DebugLogsEnabled = enabled;
                if (enabled)
                {
                    DebugLog.Write("[SETTINGS] Debug logs => enabled");
                }
            }
            catch (Exception ex)
            {
                MessageBox.Show($"Unable to update debug log settings. Error: {ex.Message}",
                                "Error",
                                MessageBoxButton.OK,
                                MessageBoxImage.Error);
            }
        }
        private void OnCloseIconClicked(object sender, MouseButtonEventArgs e)
        {
            this.Close();
        }
    }
}
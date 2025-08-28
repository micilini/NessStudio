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

namespace NessStudio.View.AboutScreen
{
    public partial class AboutScreenWindow : Window
    {
        public AboutScreenWindow()
        {
            InitializeComponent();

            AppId.Text = ((App)Application.Current).ApplicationIdentifier;
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

        private void OnCloseIconClicked(object sender, MouseButtonEventArgs e)
        {
            this.Close();
        }
    }
}

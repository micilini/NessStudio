using NessStudio.View.HomeScreen;
using NessStudio.ViewModel;
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


namespace NessStudio.View.SplashScreen
{
    public partial class SplashScreenWindow : Window
    {

        private readonly SplashScreenWindowVM viewModel;

        public SplashScreenWindow()
        {
            InitializeComponent();

            viewModel = new SplashScreenWindowVM();
            DataContext = viewModel;

            viewModel.OnLoadingComplete += OnLoadingComplete;
        }

        private async void Window_ContentRendered(object sender, EventArgs e)
        {
            await viewModel.InitializeAsync();
        }

        private void OnLoadingComplete()
        {
            Application.Current.Dispatcher.Invoke(() =>
            {
                HomeScreenWindow home = new HomeScreenWindow();
                home.Show();
                Close();
            });
        }

    }
}

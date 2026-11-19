using NessStudio.ViewModel.Helpers;
using System;
using System.Collections.Generic;
using System.ComponentModel;
using System.Linq;
using System.Text;
using System.Threading.Tasks;
namespace NessStudio.ViewModel
{
    public class SplashScreenWindowVM : INotifyPropertyChanged
    {
        public event PropertyChangedEventHandler PropertyChanged;
        protected virtual void OnPropertyChanged(string propertyName)
        {
            PropertyChanged?.Invoke(this, new PropertyChangedEventArgs(propertyName));
        }
        public event Action OnLoadingComplete;
        public async Task InitializeAsync()
        {
            try
            {
                var services = ((App)System.Windows.Application.Current).Services;
                await Task.Run(services.LoadConfiguration);
                await Task.Delay(2020);
                OnLoadingComplete?.Invoke();
            }
            catch (Exception ex)
            {
                MessageBox.Show("App settings could not be loaded.\n" + UserError.Message(ex),
                    "Error", System.Windows.MessageBoxButton.OK, System.Windows.MessageBoxImage.Error);
                System.Windows.Application.Current.Shutdown(-1);
            }
        }
    }
}

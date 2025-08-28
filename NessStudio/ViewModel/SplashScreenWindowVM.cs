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
            await Task.Run(() =>
            {
                StartAppConfiguration startupConfig = new StartAppConfiguration();
                startupConfig.CheckAndCreateDatabase();

                for (int i = 0; i <= 100; i++)
                {
                    Thread.Sleep(20);
                }
            });

            OnLoadingComplete?.Invoke();
        }
    }
}

using NessStudio.Models;
using NessStudio.View.HomeScreen;
using NessStudio.ViewModel.Commands;
using System;
using System.Collections.Generic;
using System.ComponentModel;
using System.Linq;
using System.Text;
using System.Threading.Tasks;

namespace NessStudio.ViewModel
{
    public class MenuControlVM : INotifyPropertyChanged
    {
        public event PropertyChangedEventHandler? PropertyChanged;

        private void OnPropertyChanged(string propertyName)
        {
            PropertyChanged?.Invoke(this, new PropertyChangedEventArgs(propertyName));
        }

        public HomeScreenWindowVM HomeScreenWindowVM { get; set; }

        public NewProjectCommand NewProjectCommand { get; set; }
        public OpenProjectCommand OpenProjectCommand { get; set; }
        public StartRecordingCommand StartRecordingCommand { get; set; }
        public AboutAppCommand AboutAppCommand { get; set; }

        public MenuControlVM(HomeScreenWindowVM HS)
        {
            HomeScreenWindowVM = HS;

            NewProjectCommand = new NewProjectCommand(this);
            OpenProjectCommand = new OpenProjectCommand(this);
            StartRecordingCommand = new StartRecordingCommand(this);
            AboutAppCommand = new AboutAppCommand(this);
        }

        public void HandleButtonAction(MenuAction action)
        {
            if (HomeScreenWindowVM == null) return;

            HomeScreenWindowVM.HandleMenuAction(action);
        }
    }
}

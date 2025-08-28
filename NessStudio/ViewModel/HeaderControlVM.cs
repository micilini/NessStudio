using NessStudio.Models;
using NessStudio.View.HomeScreen;
using NessStudio.ViewModel.Commands;
using System;
using System.Collections.Generic;
using System.ComponentModel;
using System.Linq;
using System.Text;
using System.Threading.Tasks;
using System.Windows;

namespace NessStudio.ViewModel
{
    public class HeaderControlVM : INotifyPropertyChanged
    {
        public event PropertyChangedEventHandler? PropertyChanged;

        private void OnPropertyChanged(string propertyName)
        {
            PropertyChanged?.Invoke(this, new PropertyChangedEventArgs(propertyName));
        }

        public HomeScreenWindowVM HomeScreenWindowVM { get; set; }
        public MinimizeApplicationCommand MinimizeApplicationCommand { get; set; }

        public MaximizeApplicationCommand MaximizeApplicationCommand { get; set; }
        public CloseApplicationCommand CloseApplicationCommand { get; set; }

        private string _maximizedIcon = "/Assets/Images/maximize-icon.png";
        public string MaximizedIcon
        {
            get => _maximizedIcon;
            set
            {
                _maximizedIcon = value;
                OnPropertyChanged(nameof(MaximizedIcon));
            }
        }

        public HeaderControlVM(HomeScreenWindowVM HS)
        {
            HomeScreenWindowVM = HS;

            MinimizeApplicationCommand = new MinimizeApplicationCommand(this);
            CloseApplicationCommand = new CloseApplicationCommand(this);
            MaximizeApplicationCommand = new MaximizeApplicationCommand(this);
        }

        public void MinimizeApplication()
        {
            HomeScreenWindowVM?.HandleWindowAction(WindowAction.Minimize);
        }

        public void MaximizeApplication()
        {
            if (MaximizedIcon == "/Assets/Images/maximize-icon.png")
            {
                MaximizedIcon = "/Assets/Images/windowed-icon.png";
            }
            else
            {
                MaximizedIcon = "/Assets/Images/maximize-icon.png";
            }
            HomeScreenWindowVM?.HandleWindowAction(WindowAction.Maximize);
        }

        public void CloseApplication()
        {
            HomeScreenWindowVM?.HandleWindowAction(WindowAction.Close);
        }

    }
}

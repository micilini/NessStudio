using NessStudio.Components.Header;
using NessStudio.Components.Menu;
using NessStudio.Components.RecentProjects;
using NessStudio.Models;
using NessStudio.View.AboutScreen;
using NessStudio.View.HomeScreen;
using NessStudio.View.RecordingScreen;
using System;
using System.Collections.Generic;
using System.ComponentModel;
using System.Linq;
using System.Text;
using System.Threading.Tasks;
using System.Windows;

namespace NessStudio.ViewModel
{
    public class HomeScreenWindowVM : INotifyPropertyChanged
    {
        public event PropertyChangedEventHandler? PropertyChanged;

        private void OnPropertyChanged(string propertyName)
        {
            PropertyChanged?.Invoke(this, new PropertyChangedEventArgs(propertyName));
        }

        public HomeScreenWindow HomeScreenWindow { get; set; }

        private object headerContent;
        public object HeaderContent
        {
            get => headerContent;
            set
            {
                headerContent = value;
                OnPropertyChanged("HeaderContent");
            }
        }

        private object menuContent;
        public object MenuContent
        {
            get => menuContent;
            set
            {
                menuContent = value;
                OnPropertyChanged("MenuContent");
            }
        }

        private object projectsContent;
        public object ProjectsContent
        {
            get => projectsContent;
            set
            {
                projectsContent = value;
                OnPropertyChanged("ProjectsContent");
            }
        }
        public HomeScreenWindowVM(HomeScreenWindow homeScreen)
        {
            HomeScreenWindow = homeScreen;

            headerContent = new HeaderControl(this);
            menuContent = new MenuControl(this);
            projectsContent = new RecentProjectsControl(this);

        }

        public void HandleWindowAction(WindowAction action)
        {
            if (HomeScreenWindow == null) return;

            switch (action)
            {
                case WindowAction.Minimize:
                    HomeScreenWindow.WindowState = System.Windows.WindowState.Minimized;
                    break;
                case WindowAction.Maximize:
                    MaximizeOrRestaureBounds();
                    break;
                case WindowAction.Close:
                    HomeScreenWindow.Close();
                    break;
            }
        }

        public void HandleMenuAction(MenuAction action)
        {
            if (HomeScreenWindow == null) return;

            switch (action)
            {
                case MenuAction.NewProject:
                    ShowNotAvaiableMessage();
                    break;

                case MenuAction.OpenProject:
                    ShowNotAvaiableMessage();
                    break;

                case MenuAction.StartRecording:
                    OpenRecordingScreen();
                    break;

                case MenuAction.AboutApplication:
                    ShowAboutWindow(); 
                    break;
            }
        }

        private void ShowNotAvaiableMessage()
        {
            MessageBox.Show(
                "This feature is not available in the current version.",
                "Warning",
                MessageBoxButton.OK,
                MessageBoxImage.Warning
            );

        }

        private void ShowAboutWindow()
        {
            var owner = Application.Current?.Windows
                .OfType<Window>()
                .FirstOrDefault(w => w.IsActive) ?? Application.Current?.MainWindow;

            var about = new AboutScreenWindow
            {
                WindowStartupLocation = WindowStartupLocation.CenterOwner
            };

            if (owner != null && !ReferenceEquals(owner, about))
                about.Owner = owner;

            about.ShowDialog();
        }

        private void OpenRecordingScreen()
        {
            HomeScreenWindow.Hide();

            var recording = new RecordingScreenWindow
            {
                Owner = HomeScreenWindow,
                WindowStartupLocation = WindowStartupLocation.CenterOwner
            };

            recording.Show();

            if (Application.Current.MainWindow == HomeScreenWindow)
                Application.Current.MainWindow = recording;

            HomeScreenWindow.Dispatcher.BeginInvoke(new Action(() =>
            {
                recording.Owner = null;
                HomeScreenWindow.Close();
            }));
        }

        private void MaximizeOrRestaureBounds()
        {
            if (HomeScreenWindow.WindowState == System.Windows.WindowState.Maximized)
            {
                var rb = HomeScreenWindow.RestoreBounds;
                HomeScreenWindow.WindowState = System.Windows.WindowState.Normal;
            }
            else
            {
                HomeScreenWindow.WindowState = System.Windows.WindowState.Maximized;
            }
        }

    }
}

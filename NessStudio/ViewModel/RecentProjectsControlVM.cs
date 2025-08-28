using MaterialDesignThemes.Wpf;
using NessStudio.Components.RecentProjects;
using NessStudio.Models;
using NessStudio.ViewModel.Commands;
using System;
using System.Collections.Generic;
using System.Collections.ObjectModel;
using System.ComponentModel;
using System.Diagnostics;
using System.IO;
using System.Linq;
using System.Text;
using System.Threading.Tasks;
using System.Windows;
using System.Windows.Controls;
using System.Windows.Data;
using System.Windows.Media;
using System.Windows.Media.Imaging;

namespace NessStudio.ViewModel
{
    public class RecentProjectsControlVM : INotifyPropertyChanged
    {
        public event PropertyChangedEventHandler? PropertyChanged;

        private void OnPropertyChanged(string propertyName)
        {
            PropertyChanged?.Invoke(this, new PropertyChangedEventArgs(propertyName));
        }

        public HomeScreenWindowVM HomeScreenWindowVM { get; set; }
        public RecentProjectsControl RecentProjectsControl { get; set; }

        public OpenFolderRecordingCommand OpenFolderRecordingCommand { get; set; }

        private bool _isLoadingProjectBlockVisible = true;
        public bool IsLoadingProjectBlockVisible
        {
            get => _isLoadingProjectBlockVisible;
            set { _isLoadingProjectBlockVisible = value; OnPropertyChanged(nameof(IsLoadingProjectBlockVisible)); }
        }

        private bool _isNoneProjectBlockVisible = false;
        public bool IsNoneProjectBlockVisible
        {
            get => _isNoneProjectBlockVisible;
            set { _isNoneProjectBlockVisible = value; OnPropertyChanged(nameof(IsNoneProjectBlockVisible)); }
        }

        private bool _isProjectBlockVisible = false;
        public bool IsProjectBlockVisible
        {
            get => _isProjectBlockVisible;
            set { _isProjectBlockVisible = value; OnPropertyChanged(nameof(IsProjectBlockVisible)); }
        }

        private ObservableCollection<ProjectsModel> _recentProjects = new();
        public ObservableCollection<ProjectsModel> RecentProjects
        {
            get => _recentProjects;
            set
            {
                _recentProjects = value;
                OnPropertyChanged(nameof(RecentProjects));
            }
        }
        public RecentProjectsControlVM(HomeScreenWindowVM HS, RecentProjectsControl recentProjects)
        {
            HomeScreenWindowVM = HS;
            RecentProjectsControl = recentProjects;

            OpenFolderRecordingCommand = new OpenFolderRecordingCommand(this);

            recentProjects.Loaded += UserControl_Loaded;
        }
        private async void UserControl_Loaded(object sender, RoutedEventArgs e)
        {
            try
            {
                IsLoadingProjectBlockVisible = true;
                IsNoneProjectBlockVisible = false;
                IsProjectBlockVisible = false;
                RecentProjects.Clear();

                var items = await Task.Run(() =>
                {
                    var conn = ((App)Application.Current).DBConnection;
                    conn.CreateTable<ProjectsModel>();
                    return conn.Table<ProjectsModel>()
                               .Where(p => !p.IsDeleted)
                               .OrderByDescending(p => p.UpdatedAt)
                               .Take(12)
                               .ToList();
                });

                if (items == null || items.Count == 0)
                {
                    IsLoadingProjectBlockVisible = false;
                    IsNoneProjectBlockVisible = true;
                    IsProjectBlockVisible = false;
                    return;
                }

                await RecentProjectsControl.RenderProjectsAsync(items, clearFirst: true);

                IsLoadingProjectBlockVisible = false;
                IsNoneProjectBlockVisible = false;
                IsProjectBlockVisible = true;
            }
            catch (Exception ex)
            {
                IsLoadingProjectBlockVisible = false;
                IsNoneProjectBlockVisible = true;
                IsProjectBlockVisible = false;
                MessageBox.Show($"Failed to load projects:\n{ex.Message}", "Recent Projects",
                    MessageBoxButton.OK, MessageBoxImage.Warning);
            }
        }

        public async Task<bool> DeleteProjectAsync(ProjectsModel model)
        {
            try
            {
                var conn = ((App)Application.Current).DBConnection;
                conn.RunInTransaction(() =>
                {
                    conn.CreateTable<ProjectsModel>();
                    conn.Delete(model);
                });

                string folderToDelete = model?.ProjectFolderPath;
                if (string.IsNullOrWhiteSpace(folderToDelete) || !Directory.Exists(folderToDelete))
                {
                    var docs = Environment.GetFolderPath(Environment.SpecialFolder.MyDocuments);
                    folderToDelete = System.IO.Path.Combine(docs, "NessStudio", "Recordings", SanitizeFolderName(model?.Title));
                }
                await Task.Run(() => DeleteDirectorySafe(folderToDelete));

                return true;
            }
            catch
            {
                return false;
            }
        }

        private static void DeleteDirectorySafe(string folder)
        {
            try
            {
                if (string.IsNullOrWhiteSpace(folder) || !Directory.Exists(folder)) return;

                foreach (var file in Directory.GetFiles(folder, "*", SearchOption.AllDirectories))
                {
                    try
                    {
                        var attrs = File.GetAttributes(file);
                        if ((attrs & FileAttributes.ReadOnly) == FileAttributes.ReadOnly)
                            File.SetAttributes(file, attrs & ~FileAttributes.ReadOnly);
                    }
                    catch { }
                }

                Directory.Delete(folder, recursive: true);
            }
            catch
            {

            }
        }

        private static string SanitizeFolderName(string raw)
        {
            if (string.IsNullOrWhiteSpace(raw)) return "Untitled Recording";
            var invalid = System.IO.Path.GetInvalidFileNameChars();
            var clean = new string(raw.Select(ch => invalid.Contains(ch) ? '_' : ch).ToArray());
            return clean.Trim();
        }

        public static T FindAncestor<T>(DependencyObject current) where T : DependencyObject
        {
            while (current != null)
            {
                if (current is T match) return match;
                current = VisualTreeHelper.GetParent(current);
            }
            return null;
        }
        
        public static void OpenInExplorer(string folder)
        {
            if (string.IsNullOrWhiteSpace(folder) || !Directory.Exists(folder)) return;

            var psi = new System.Diagnostics.ProcessStartInfo
            {
                FileName = "explorer.exe",
                Arguments = $"\"{folder}\"",
                UseShellExecute = true
            };
            System.Diagnostics.Process.Start(psi);
        }

        public async void OpenFolder_Click()
        {
            try
            {
                string documents = Environment.GetFolderPath(Environment.SpecialFolder.MyDocuments);

                string targetPath = System.IO.Path.Combine(documents, "NessStudio", "Recordings");

                if (!Directory.Exists(targetPath))
                {
                    Directory.CreateDirectory(targetPath);
                }

                Process.Start("explorer.exe", targetPath);
            }
            catch (Exception ex)
            {
                MessageBox.Show($"Error when open folder:\n{ex.Message}", "Error",
                    MessageBoxButton.OK, MessageBoxImage.Error);
            }
        }
    }
}

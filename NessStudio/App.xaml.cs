using SQLite;
using System.Configuration;
using System.Data;
using System.IO;
using System.Windows;
using System.Windows.Interop;
using System.Windows.Media;

namespace NessStudio
{
    public partial class App : Application
    {
        private static Mutex _appMutex;
        public string KeyDatabase = string.Empty;
        public string DatabaseFileName = "nesstudio.dll";
        public string ApplicationVersion = "1.0.0";
        public string ApplicationIdentifier { get; set; }
        public string AppLanguage { get; set; }

        public App()
        {
            this.DispatcherUnhandledException += (s, e) =>
            {
                MessageBox.Show("Global Error:\n\n" + e.Exception, "Fatal",
                    MessageBoxButton.OK, MessageBoxImage.Error);
                Shutdown(-1);
            };

            AppDomain.CurrentDomain.UnhandledException += (s, e) =>
            {
                MessageBox.Show("Global Error:\n\n" + e.ExceptionObject, "Fatal",
                    MessageBoxButton.OK, MessageBoxImage.Error);
                Shutdown(-1);
            };
        }

        public SQLiteConnection DBConnection { get; set; }

        protected override void OnStartup(StartupEventArgs e)
        {
            string appName = @"Local\NessStudio";
            bool createdNew;

            _appMutex = new Mutex(true, appName, out createdNew);

            if (!createdNew)
            {
                Environment.Exit(0);
            }

            RenderOptions.ProcessRenderMode = RenderMode.SoftwareOnly;

            CreateApplicationFolderIfNeeded("NessStudio");
            CreateApplicationDocumentsFolderIfNeeded("NessStudio");
            CreateApplicationDocumentsFolderIfNeeded("NessStudio/Recordings");

            base.OnStartup(e);
        }

        protected override void OnExit(ExitEventArgs e)
        {
            _appMutex?.ReleaseMutex();
            _appMutex = null;

            base.OnExit(e);
        }

        public void CreateApplicationFolderIfNeeded(string appName)
        {
            string appFolderPath = Path.Combine(
                Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData),
                appName
            );

            if (!Directory.Exists(appFolderPath))
            {
                Directory.CreateDirectory(appFolderPath);
            }

        }

        public void CreateApplicationDocumentsFolderIfNeeded(string relativePath)
        {
            if (string.IsNullOrWhiteSpace(relativePath))
                throw new ArgumentException("relativePath must not be null or empty.", nameof(relativePath));

            string docsRoot = Environment.GetFolderPath(Environment.SpecialFolder.MyDocuments);

            string normalized = relativePath
                .Replace('/', Path.DirectorySeparatorChar)
                .Replace('\\', Path.DirectorySeparatorChar)
                .TrimStart(Path.DirectorySeparatorChar);

            string fullPath = Path.Combine(docsRoot, normalized);

            if (!Directory.Exists(fullPath))
            {
                Directory.CreateDirectory(fullPath);
            }
        }

    }

}

using SQLite;
using System.Configuration;
using System.Data;
using System.IO;
using System.Reflection;
using System.Windows;
using System.Windows.Interop;
using System.Windows.Media;
using NessStudio.Composition;
namespace NessStudio
{
    public partial class App : Application
    {
        private static Mutex _appMutex;
        public string DatabaseFileName = "nesstudio.dll";
        /// <summary>Gets the product version embedded in the application assembly.</summary>
        public static string DisplayVersion { get; } =
            typeof(App).Assembly.GetCustomAttribute<AssemblyInformationalVersionAttribute>()?
                .InformationalVersion.Split('+')[0]
            ?? typeof(App).Assembly.GetName().Version?.ToString(3)
            ?? "Unknown";
        /// <summary>Gets the same version shown in the application's windows.</summary>
        public string ApplicationVersion => DisplayVersion;
        public string ApplicationIdentifier { get; set; }
        public string AppLanguage { get; set; }
        public bool DebugLogsEnabled { get; set; } = false;
        public int RecordingTimerSeconds { get; set; } = 3;
        public int RecordingFps { get; set; } = 30;
        /// <summary>Gets or sets the recording execution policy selected in the frontend.</summary>
        public NessStudio.Recording.Engines.RecordingExecutionMode RecordingExecutionMode { get; set; }
        public string RecordingPreferencesFileName = "recording-preferences.json";
        public string RecordingPreferencesFilePath
        {
            get
            {
                return Path.Combine(Options.DataDirectory, RecordingPreferencesFileName);
            }
        }
        public ApplicationOptions Options { get; }
        public ApplicationServices Services { get; }
        public App() : this(ApplicationOptions.ForCurrentUser()) { }
        public App(ApplicationOptions options)
        {
            Options = options ?? throw new ArgumentNullException(nameof(options));
            RenderOptions.ProcessRenderMode = options.PresentationMode;
            // Apply a persisted software presentation policy before constructing
            // the first WPF window, including the startup splash. Read no devices.
            try
            {
                var preferences = new FileInfo(RecordingPreferencesFilePath);
                if (preferences.Exists && preferences.Length <= 16384)
                {
                    using var json = System.Text.Json.JsonDocument.Parse(File.ReadAllText(preferences.FullName));
                    if (json.RootElement.TryGetProperty("ExecutionMode", out var mode) && mode.ValueKind == System.Text.Json.JsonValueKind.Number && mode.TryGetInt32(out int value) &&
                        value == (int)NessStudio.Recording.Engines.RecordingExecutionMode.FullSoftware)
                        RenderOptions.ProcessRenderMode = RenderMode.SoftwareOnly;
                }
            }
            catch (Exception error) when (error is IOException or UnauthorizedAccessException or System.Text.Json.JsonException) { }
            Services = new ApplicationServices(this);
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
        public SQLiteConnection? DBConnection { get; set; }
        protected override void OnStartup(StartupEventArgs e)
        {
            string appName = Options.InstanceMutexName;
            bool createdNew;
            _appMutex = new Mutex(true, appName, out createdNew);
            if (!createdNew)
            {
                Environment.Exit(0);
            }
            Directory.CreateDirectory(Options.DataDirectory);
            Directory.CreateDirectory(Options.DocumentsDirectory);
            Directory.CreateDirectory(Path.Combine(Options.DocumentsDirectory, "Recordings"));
            base.OnStartup(e);
        }
        protected override void OnExit(ExitEventArgs e)
        {
            Services.Dispose();
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

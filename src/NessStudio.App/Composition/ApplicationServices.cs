using NessStudio.Recording.Engines;
using NessStudio.ViewModel.Helpers;
using System.IO;
using NessStudio.Interop.Native;

namespace NessStudio.Composition;

/// <summary>Composes the native recorder and frontend services with explicit dependencies.</summary>
public sealed class ApplicationServices : IDisposable
{
    private readonly App _app;
    private NativeBuildInformation? _nativeBuild;
    /// <summary>Gets the native bootstrap identity when the packaged engine loaded successfully.</summary>
    public NativeBuildInformation? NativeEngineBuild => _nativeBuild;
    /// <summary>Gets a diagnostic error when the required native runtime cannot be loaded.</summary>
    public string? NativeEngineError { get; private set; }
    /// <summary>Initializes services without opening a database or device.</summary>
    public ApplicationServices(App app)
    {
        _app = app;
        DebugLog.Configure(Path.Combine(app.Options.DataDirectory, "Logs", "recording-debug.log"), () => app.DebugLogsEnabled,
            Path.Combine(app.Options.DataDirectory, "Logs", "recording-errors.log"));
        DatabaseHelper.Configure(() => app.DBConnection ?? throw new InvalidOperationException("The application database has not been initialized."));
        RecordingPreferences = new RecordingPreferencesService(() => app.RecordingPreferencesFilePath, DebugLog.Logger);
        RecorderFactory = new RecordingEngineFactory(AppContext.BaseDirectory, DebugLog.Logger);
    }
    /// <summary>Gets preferences backed by the supplied profile path.</summary>
    public RecordingPreferencesService RecordingPreferences { get; }
    /// <summary>Gets the native recorder factory with explicit execution policy.</summary>
    public IRecorderEngineFactory RecorderFactory { get; }
    /// <summary>Loads data on the startup worker and publishes the frontend settings.</summary>
    public void LoadConfiguration()
    {
        var startup = new StartAppConfiguration(new ApplicationStorageOptions(_app.Options.DataDirectory, _app.DatabaseFileName), RecordingPreferences);
        var result = startup.Initialize();
        _app.DBConnection = result.Connection;
        _app.ApplicationIdentifier = result.Settings.ApplicationIdentifier;
        _app.AppLanguage = result.Settings.Language;
        _app.DebugLogsEnabled = result.Settings.EnableDebugLogs;
        _app.RecordingTimerSeconds = result.RecordingPreferences.TimerSeconds;
        _app.RecordingFps = result.RecordingPreferences.RecordingFps;
        _app.RecordingExecutionMode = result.RecordingPreferences.ExecutionMode;
        try
        {
            using var bootstrap = NativeEngine.Open(AppContext.BaseDirectory);
            _nativeBuild = bootstrap.Build;
            NativeEngineError = null;
        }
        catch (Exception error) when (error is FileNotFoundException or DllNotFoundException or BadImageFormatException or EntryPointNotFoundException or NativeEngineException)
        {
            NativeEngineError = UserError.Message(error);
            DebugLog.Write("[NativeBootstrap] " + UserError.Message(error));
        }
    }
    /// <inheritdoc />
    public void Dispose()
    {
        DatabaseConnectionManager.CloseConnection();
        DebugLog.Dispose();
    }
}

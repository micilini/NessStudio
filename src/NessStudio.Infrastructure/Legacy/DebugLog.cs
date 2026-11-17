using NessStudio.Diagnostics;

namespace NessStudio.ViewModel.Helpers;

/// <summary>Preserves the legacy logging facade with an explicitly supplied sink and policy.</summary>
public static class DebugLog
{
    private static FileDiagnosticLog? _fileLog;
    /// <summary>Gets the configured diagnostic port for components in other assemblies.</summary>
    public static IDiagnosticLog Logger => _fileLog ?? (IDiagnosticLog)NullDiagnosticLog.Instance;

    /// <summary>Configures logging at application composition time, before workers start.</summary>
    public static void Configure(string path, Func<bool> isEnabled)
    {
        _fileLog?.Dispose();
        _fileLog = new FileDiagnosticLog(path, isEnabled);
    }

    public static string GetPath() => _fileLog?.Path
        ?? throw new InvalidOperationException("Logging must be configured by the application composition root.");
    public static void Write(string message) => Logger.Write(message);
    /// <summary>Persists failures even when verbose diagnostics are disabled.</summary>
    public static void WriteError(string message) => _fileLog?.WriteError(message);
    public static void Flush() => _fileLog?.Flush();
    public static void Dispose() => _fileLog?.Dispose();

    private sealed class FileDiagnosticLog(string path, Func<bool> isEnabled) : IDiagnosticLog, IDisposable
    {
        private readonly object _sync = new();
        private StreamWriter? _writer;
        private bool _disposed;
        internal string Path { get; } = System.IO.Path.GetFullPath(path);

        public void Write(string message)
        {
            if (!isEnabled()) return;
            WriteError(message);
        }
        internal void WriteError(string message)
        {
            lock (_sync)
            {
                if (_disposed) return;
                try
                {
                    if (_writer is null)
                    {
                        Directory.CreateDirectory(System.IO.Path.GetDirectoryName(Path)!);
                        _writer = new StreamWriter(Path, append: true, System.Text.Encoding.UTF8) { AutoFlush = true };
                    }
                    _writer.WriteLine($"[{DateTime.Now:yyyy-MM-dd HH:mm:ss.fff}] {message}");
                }
                catch (IOException) { /* Optional diagnostics must not break media operations. */ }
                catch (UnauthorizedAccessException) { }
            }
        }
        public void Flush()
        {
            lock (_sync) { try { _writer?.Flush(); } catch (IOException) { } }
        }
        public void Dispose()
        {
            lock (_sync)
            {
                if (_disposed) return;
                _disposed = true;
                try { _writer?.Dispose(); } catch (IOException) { }
                finally { _writer = null; }
            }
        }
    }
}

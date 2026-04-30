using System;
using System.IO;
using System.Windows;

namespace NessStudio.ViewModel.Helpers
{
    public static class DebugLog
    {
        
        private static StreamWriter _writer;
        private static readonly object _lock = new object();
        private static bool _initialized;

        public static string GetPath()
        {
            string dir = Path.Combine(
                Environment.GetFolderPath(Environment.SpecialFolder.MyDocuments),
                "NessStudio",
                "Logs");
            Directory.CreateDirectory(dir);
            return Path.Combine(dir, "recording-debug.log");
        }

        private static void EnsureWriter()
        {
            if (_initialized) return;
            try
            {
                var path = GetPath();
                _writer = new StreamWriter(path, append: true, encoding: System.Text.Encoding.UTF8)
                {
                    AutoFlush = true 
                };
                _initialized = true;
            }
            catch { }
        }

        public static void Write(string message)
        {
            try
            {
                var app = Application.Current as App;
                if (app != null && !app.DebugLogsEnabled)
                    return;

                lock (_lock)
                {
                    EnsureWriter();
                    _writer?.WriteLine($"[{DateTime.Now:yyyy-MM-dd HH:mm:ss.fff}] {message}");
                }
            }
            catch { }
        }

        
        
        public static void Flush()
        {
            try
            {
                lock (_lock)
                {
                    _writer?.Flush();
                }
            }
            catch { }
        }

        
        public static void Dispose()
        {
            try
            {
                lock (_lock)
                {
                    _writer?.Flush();
                    _writer?.Dispose();
                    _writer = null;
                    _initialized = false;
                }
            }
            catch { }
        }
        
    }
}
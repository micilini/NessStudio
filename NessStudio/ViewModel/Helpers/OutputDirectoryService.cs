using NessStudio.Models;
using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Text;
using System.Threading.Tasks;

namespace NessStudio.ViewModel.Helpers
{
    public static class OutputDirectoryService
    {
        public static string EnsureDefaultRoot()
        {
            var root = Path.Combine(
                Environment.GetFolderPath(Environment.SpecialFolder.MyDocuments),
                "NessStudio", "Recordings");
            Directory.CreateDirectory(root);
            return root;
        }

        public static string EnsureBaseDir(string baseDir)
        {
            if (string.IsNullOrWhiteSpace(baseDir))
                throw new ArgumentException("Base directory is required.", nameof(baseDir));
            Directory.CreateDirectory(baseDir);
            return baseDir;
        }

        public static RecordingOutputPaths BuildPaths(string root = null, DateTime? timestamp = null)
        {
            root ??= EnsureDefaultRoot();
            var ts = (timestamp ?? DateTime.Now).ToString("yyyyMMdd_HHmmss");
            var sessionDir = Path.Combine(root, ts);
            Directory.CreateDirectory(sessionDir);
            return new RecordingOutputPaths(sessionDir);
        }
    }
}

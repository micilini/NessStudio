using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Text;
using System.Threading.Tasks;

namespace NessStudio.ViewModel.Helpers
{
    public static class FileCleanup
    {
        public static void DeleteTxtAndLogArtifactsInFolder(string folder, bool recurse = true)
        {
            if (string.IsNullOrWhiteSpace(folder) || !Directory.Exists(folder))
                return;

            var opt = recurse ? SearchOption.AllDirectories : SearchOption.TopDirectoryOnly;

            foreach (var file in Directory.GetFiles(folder, "*.txt", opt))
            {
                try { File.Delete(file); }
                catch {  }
            }

            // deleta *.log
            foreach (var file in Directory.GetFiles(folder, "*.log", opt))
            {
                try { File.Delete(file); }
                catch {  }
            }
        }

        public static void DeleteTxtAndLogArtifactsFromDeliver(string deliverPath, bool recurse = false)
        {
            if (string.IsNullOrWhiteSpace(deliverPath))
                return;

            var folder = Path.GetDirectoryName(deliverPath);
            if (string.IsNullOrEmpty(folder)) return;

            DeleteTxtAndLogArtifactsInFolder(folder, recurse);
        }
    }
}

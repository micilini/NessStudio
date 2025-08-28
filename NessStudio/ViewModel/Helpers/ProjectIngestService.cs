using NessStudio.Models;
using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.Drawing;
using System.Drawing.Imaging;
using System.IO;
using System.Linq;
using System.Text;
using System.Threading.Tasks;

namespace NessStudio.ViewModel.Helpers
{
    public static class ProjectIngestService
    {
        private static string FfmpegPath =>
            Path.Combine(AppDomain.CurrentDomain.BaseDirectory, "Modules", "ffmpeg.exe");

        public static async Task<ProjectsModel> ProcessAsync(string projectFolder)
        {
            if (string.IsNullOrWhiteSpace(projectFolder) || !Directory.Exists(projectFolder))
                throw new DirectoryNotFoundException($"Folder not found: {projectFolder}");

            string previewPath = Path.Combine(projectFolder, "preview.png");

            var mp4 = Directory.EnumerateFiles(projectFolder, "*.mp4", SearchOption.TopDirectoryOnly)
                .OrderByDescending(f => Path.GetFileName(f).Contains("_final", StringComparison.OrdinalIgnoreCase))
                .ThenBy(f => f, StringComparer.OrdinalIgnoreCase)
                .FirstOrDefault();

            if (mp4 != null && File.Exists(FfmpegPath))
            {
                var psi = new ProcessStartInfo
                {
                    FileName = FfmpegPath,
                    Arguments = $"-y -hide_banner -loglevel error -ss 0 -i \"{mp4}\" -frames:v 1 \"{previewPath}\"",
                    UseShellExecute = false,
                    CreateNoWindow = true
                };
                using var p = Process.Start(psi);
                await Task.Run(() => p.WaitForExit());
            }

            long sizeBytes = GetFolderSize(projectFolder);

            var dirInfo = new DirectoryInfo(projectFolder);
            var model = new ProjectsModel
            {
                Title = dirInfo.Name,
                ProjectFolderPath = projectFolder,
                ThumbnailPath = File.Exists(previewPath) ? previewPath : string.Empty,
                FileSizeBytes = sizeBytes,
                CreatedAt = DateTime.Now,
                UpdatedAt = DateTime.Now,
                LastOpenedAt = DateTime.Now
            };

            DatabaseHelper.Insert(model);

            return model;
        }

        private static long GetFolderSize(string folder)
        {
            long total = 0;
            foreach (var f in Directory.EnumerateFiles(folder, "*", SearchOption.AllDirectories))
            {
                try { total += new FileInfo(f).Length; }
                catch { }
            }
            return total;
        }
    }
}

using NessStudio.Models;
using System;
using System.Collections.Generic;
using System.Drawing.Imaging;
using System.IO;
using System.Linq;
using System.Threading.Tasks;
namespace NessStudio.ViewModel.Helpers
{
    public static class ProjectIngestService
    {
        private static void ReportSaveProgress(
            IProgress<RecordingSaveProgress>? progress,
            string title,
            string message,
            int percent,
            int currentStep,
            int totalSteps,
            bool isIndeterminate = false)
        {
            if (progress == null)
                return;
            progress.Report(new RecordingSaveProgress
            {
                Title = title,
                Message = message,
                Percent = percent,
                CurrentStep = currentStep,
                TotalSteps = totalSteps,
                IsIndeterminate = isIndeterminate
            });
        }
        public static async Task<ProjectsModel> ProcessAsync(
    string projectFolder,
    IProgress<RecordingSaveProgress>? progress = null)
        {
            const int totalSteps = 8;
            if (string.IsNullOrWhiteSpace(projectFolder) || !Directory.Exists(projectFolder))
                throw new DirectoryNotFoundException($"Folder not found: {projectFolder}");
            DebugLog.Write($"[ProjectIngest] begin | folder={projectFolder}");
            string previewPath = Path.Combine(projectFolder, "preview.png");
            var mp4 = Directory.EnumerateFiles(projectFolder, "*.mp4", SearchOption.TopDirectoryOnly)
                    .OrderBy(f =>
                    {
                        var name = Path.GetFileName(f);
                        if (name.Equals("screen_01.mp4", StringComparison.OrdinalIgnoreCase)) return 0;
                        if (name.StartsWith("screen_", StringComparison.OrdinalIgnoreCase)) return 1;
                        if (name.Equals("webcam_01.mp4", StringComparison.OrdinalIgnoreCase)) return 2;
                        if (name.StartsWith("webcam_", StringComparison.OrdinalIgnoreCase)) return 3;
                        return 9;
                    })
                    .ThenBy(f => f, StringComparer.OrdinalIgnoreCase)
                    .FirstOrDefault();
            DebugLog.Write($"[ProjectIngest] chosen mp4 => {mp4}");
            ReportSaveProgress(progress, "Saving Recording...", "Generating preview...", 95, 7, totalSteps);
            if (mp4 != null && File.Exists(mp4))
            {
                try
                {
                    double thumbnailOffsetSeconds = 2.0;

                    bool thumbOk = await Task.Run(() =>
                    {
                        DebugLog.Write($"[ProjectIngest] thumbnail begin => {mp4} | offset={thumbnailOffsetSeconds:F2}s");
                        return NessStudio.Recording.Windows.MfThumbnailer.TryWriteFramePng(
                            mp4,
                            previewPath,
                            thumbnailOffsetSeconds);
                    }).WaitAsync(TimeSpan.FromSeconds(5));
                    DebugLog.Write($"[ProjectIngest] thumbnail end => ok={thumbOk} | preview={previewPath}");
                }
                catch (TimeoutException)
                {
                    DebugLog.Write("[ProjectIngest] thumbnail TIMEOUT (5s)");
                }
                catch (Exception ex)
                {
                    DebugLog.Write("[ProjectIngest] thumbnail ERROR:\n" + ex);
                }
            }
            else
            {
                DebugLog.Write("[ProjectIngest] thumbnail skipped | no valid mp4 found");
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
            ReportSaveProgress(progress, "Saving Recording...", "Registering project...", 98, 8, totalSteps);
            DatabaseHelper.Insert(model);
            ReportSaveProgress(progress, "Saving Recording...", "Finishing...", 100, 8, totalSteps);
            DebugLog.Write($"[ProjectIngest] end | inserted={model.Title}");
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
using NessStudio.Models;
using NessStudio.Recording;
using System;
using System.Collections.Generic;
using System.Drawing.Imaging;
using System.IO;
using System.Linq;
using System.Text.Json;
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

            var videoFile = ResolvePreviewVideoFile(projectFolder);
            DebugLog.Write($"[ProjectIngest] chosen preview video => {videoFile}");

            ReportSaveProgress(progress, "Saving Recording...", "Generating preview...", 95, 7, totalSteps);

            if (videoFile != null && File.Exists(videoFile))
            {
                try
                {
                    double thumbnailOffsetSeconds = 2.0;

                    bool thumbOk = await Task.Run(() =>
                    {
                        DebugLog.Write($"[ProjectIngest] thumbnail begin => {videoFile} | offset={thumbnailOffsetSeconds:F2}s");

                        return NessStudio.Recording.Windows.MfThumbnailer.TryWriteFramePng(
                            videoFile,
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
                DebugLog.Write("[ProjectIngest] thumbnail skipped | no valid preview video found");
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

        private static string? ResolvePreviewVideoFile(string projectFolder)
        {
            var manifestVideoFile = ResolvePreviewVideoFileFromManifest(projectFolder);

            if (manifestVideoFile != null)
                return manifestVideoFile;

            var screenMkv = Path.Combine(projectFolder, "screen.mkv");

            if (File.Exists(screenMkv))
                return screenMkv;

            var webcamMp4 = Path.Combine(projectFolder, "webcam.mp4");

            if (File.Exists(webcamMp4))
                return webcamMp4;

            return Directory.EnumerateFiles(projectFolder, "*.mp4", SearchOption.TopDirectoryOnly)
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
        }

        private static string? ResolvePreviewVideoFileFromManifest(string projectFolder)
        {
            var manifestPaths = new[]
            {
                Path.Combine(projectFolder, "manifest.json"),
                Path.Combine(projectFolder, "session.manifest.json")
            };

            foreach (var manifestPath in manifestPaths)
            {
                if (!File.Exists(manifestPath))
                    continue;

                try
                {
                    var json = File.ReadAllText(manifestPath);
                    var manifest = JsonSerializer.Deserialize<SessionManifest>(json);

                    var screenFile = manifest?.Screen?.File;
                    var webcamFile = manifest?.Webcam?.File;

                    var screenPath = ResolveExistingTrackPath(projectFolder, screenFile);

                    if (screenPath != null)
                        return screenPath;

                    var webcamPath = ResolveExistingTrackPath(projectFolder, webcamFile);

                    if (webcamPath != null)
                        return webcamPath;
                }
                catch (Exception ex)
                {
                    DebugLog.Write($"[ProjectIngest] manifest preview lookup failed | manifest={manifestPath}\n" + ex);
                }
            }

            return null;
        }

        private static string? ResolveExistingTrackPath(string projectFolder, string? fileName)
        {
            if (string.IsNullOrWhiteSpace(fileName))
                return null;

            var fullPath = Path.IsPathRooted(fileName)
                ? fileName
                : Path.Combine(projectFolder, fileName);

            return File.Exists(fullPath) ? fullPath : null;
        }

        private static long GetFolderSize(string folder)
        {
            long total = 0;

            foreach (var f in Directory.EnumerateFiles(folder, "*", SearchOption.AllDirectories))
            {
                try
                {
                    total += new FileInfo(f).Length;
                }
                catch
                {
                }
            }

            return total;
        }
    }
}
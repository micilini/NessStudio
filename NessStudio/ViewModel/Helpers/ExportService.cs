using NessStudio.Recording;
using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.Globalization;
using System.IO;
using System.Linq;
using System.Text;
using System.Text.Json;
using System.Threading;
using System.Threading.Tasks;

namespace NessStudio.ViewModel.Helpers
{
    public enum RecordingExportMode
    {
        SingleFile,
        SeparateTracks
    }

    public enum RecordingExportVideoLayout
    {
        NoVideo,
        ScreenOnly,
        ScreenWebcamPictureInPicture,
        ScreenWebcamSideBySide,
        WebcamOnly
    }

    public enum RecordingExportAudioMode
    {
        NoAudio,
        MicrophoneOnly,
        SystemAudioOnly,
        MicrophoneAndSystemAudio
    }

    public enum RecordingExportContainer
    {
        Mp4,
        Mkv,
        Mp3,
        Wav
    }

    public enum RecordingExportQuality
    {
        Fast,
        Balanced,
        HighQuality,
        Audio128Kbps,
        Audio192Kbps,
        Audio320Kbps,
        Wav44100Hz16Bit,
        Wav48000Hz16Bit,
        Wav48000Hz24Bit
    }

    public sealed class RecordingExportRequest
    {
        public string ProjectFolder { get; set; } = string.Empty;
        public string OutputPath { get; set; } = string.Empty;
        public RecordingExportMode Mode { get; set; } = RecordingExportMode.SingleFile;
        public RecordingExportVideoLayout VideoLayout { get; set; } = RecordingExportVideoLayout.ScreenWebcamPictureInPicture;
        public RecordingExportAudioMode AudioMode { get; set; } = RecordingExportAudioMode.MicrophoneAndSystemAudio;
        public RecordingExportContainer Container { get; set; } = RecordingExportContainer.Mp4;
        public RecordingExportQuality Quality { get; set; } = RecordingExportQuality.Balanced;
        public bool ExportScreenTrack { get; set; } = true;
        public bool ExportWebcamTrack { get; set; } = true;
        public bool ExportAudioMixTrack { get; set; } = true;
    }

    public sealed class RecordingExportResult
    {
        public bool Success { get; set; }
        public string Message { get; set; } = string.Empty;
        public string OutputPath { get; set; } = string.Empty;
        public string Log { get; set; } = string.Empty;
    }

    public sealed class RecordingExportSessionInfo
    {
        public string ProjectFolder { get; set; } = string.Empty;
        public string ManifestPath { get; set; } = string.Empty;
        public SessionManifest Manifest { get; set; }
        public string? ScreenPath { get; set; }
        public string? WebcamPath { get; set; }
        public string? MicPath { get; set; }
        public string? SystemPath { get; set; }

        public bool HasScreen => !string.IsNullOrWhiteSpace(ScreenPath) && File.Exists(ScreenPath);
        public bool HasWebcam => !string.IsNullOrWhiteSpace(WebcamPath) && File.Exists(WebcamPath);
        public bool HasMic => !string.IsNullOrWhiteSpace(MicPath) && File.Exists(MicPath);
        public bool HasSystemAudio => !string.IsNullOrWhiteSpace(SystemPath) && File.Exists(SystemPath);
        public bool HasAnyVideo => HasScreen || HasWebcam;
        public bool HasAnyAudio => HasMic || HasSystemAudio;
    }

    internal sealed class ExportInput
    {
        public int Index { get; set; }
        public string Kind { get; set; } = string.Empty;
        public string Path { get; set; } = string.Empty;
        public SessionManifest.AudioTrack? AudioTrack { get; set; }
    }

    public static class ExportService
    {
        private static readonly JsonSerializerOptions ManifestJsonOptions = new()
        {
            PropertyNameCaseInsensitive = true,
            AllowTrailingCommas = true
        };

        public static string FindFFmpegPath()
        {
            var candidates = new List<string>();
            var baseDir = AppContext.BaseDirectory;

            candidates.Add(Path.Combine(baseDir, "Native", "FFmpeg", "ffmpeg.exe"));
            candidates.Add(Path.Combine(baseDir, "ffmpeg.exe"));

            var current = new DirectoryInfo(baseDir);
            for (int i = 0; i < 8 && current != null; i++)
            {
                candidates.Add(Path.Combine(current.FullName, "Native", "FFmpeg", "ffmpeg.exe"));
                candidates.Add(Path.Combine(current.FullName, "NessStudio", "Native", "FFmpeg", "ffmpeg.exe"));
                current = current.Parent;
            }

            foreach (var candidate in candidates.Distinct(StringComparer.OrdinalIgnoreCase))
            {
                if (File.Exists(candidate))
                    return candidate;
            }

            return candidates[0];
        }

        public static RecordingExportSessionInfo LoadSessionInfo(string projectFolder)
        {
            if (string.IsNullOrWhiteSpace(projectFolder))
                throw new ArgumentException("Project folder is empty.", nameof(projectFolder));

            if (!Directory.Exists(projectFolder))
                throw new DirectoryNotFoundException($"Project folder not found: {projectFolder}");

            var manifestPath = ResolveManifestPath(projectFolder);
            var json = File.ReadAllText(manifestPath);
            var manifest = JsonSerializer.Deserialize<SessionManifest>(json, ManifestJsonOptions);

            if (manifest == null)
                throw new InvalidOperationException("Unable to read the recording manifest.");

            return new RecordingExportSessionInfo
            {
                ProjectFolder = projectFolder,
                ManifestPath = manifestPath,
                Manifest = manifest,
                ScreenPath = ResolveExistingTrackPath(projectFolder, manifest.Screen?.File),
                WebcamPath = ResolveExistingTrackPath(projectFolder, manifest.Webcam?.File),
                MicPath = ResolveExistingTrackPath(projectFolder, manifest.Mic?.File),
                SystemPath = ResolveExistingTrackPath(projectFolder, manifest.System?.File)
            };
        }

        public static string BuildDefaultSingleFileOutputPath(string projectFolder, RecordingExportContainer container)
        {
            var baseName = SanitizeFileName(new DirectoryInfo(projectFolder).Name);
            var ext = container switch
            {
                RecordingExportContainer.Mkv => ".mkv",
                RecordingExportContainer.Mp3 => ".mp3",
                RecordingExportContainer.Wav => ".wav",
                _ => ".mp4"
            };
            var exportFolder = Path.Combine(projectFolder, "Exports");
            return Path.Combine(exportFolder, $"{baseName}_export{ext}");
        }

        public static string BuildDefaultSeparateTracksOutputFolder(string projectFolder)
        {
            var baseName = SanitizeFileName(new DirectoryInfo(projectFolder).Name);
            return Path.Combine(projectFolder, "Exports", $"{baseName}_tracks");
        }

        public static async Task<RecordingExportResult> ExportAsync(
            RecordingExportRequest request,
            IProgress<string>? progress = null,
            CancellationToken cancellationToken = default)
        {
            if (request == null)
                throw new ArgumentNullException(nameof(request));

            var info = LoadSessionInfo(request.ProjectFolder);

            if (request.Mode == RecordingExportMode.SeparateTracks)
                return await ExportSeparateTracksAsync(info, request, progress, cancellationToken);

            return await ExportSingleFileAsync(info, request, progress, cancellationToken);
        }

        private static async Task<RecordingExportResult> ExportSingleFileAsync(
            RecordingExportSessionInfo info,
            RecordingExportRequest request,
            IProgress<string>? progress,
            CancellationToken cancellationToken)
        {
            var ffmpegPath = FindFFmpegPath();

            if (!File.Exists(ffmpegPath))
            {
                return new RecordingExportResult
                {
                    Success = false,
                    Message = $"FFmpeg was not found at:\n{ffmpegPath}",
                    OutputPath = request.OutputPath
                };
            }

            if (string.IsNullOrWhiteSpace(request.OutputPath))
                request.OutputPath = BuildDefaultSingleFileOutputPath(info.ProjectFolder, request.Container);

            var outputDir = Path.GetDirectoryName(request.OutputPath);
            if (!string.IsNullOrWhiteSpace(outputDir))
                Directory.CreateDirectory(outputDir);

            if (IsAudioOnlyContainer(request.Container) && request.VideoLayout != RecordingExportVideoLayout.NoVideo)
                return Fail("MP3 and WAV exports are audio-only. Please choose 'No video · Audio only' before exporting to MP3 or WAV.", request.OutputPath);

            var inputs = new List<ExportInput>();
            int screenIndex = -1;
            int webcamIndex = -1;
            int micIndex = -1;
            int systemIndex = -1;

            void AddInput(string kind, string path, SessionManifest.AudioTrack? audioTrack = null)
            {
                if (inputs.Any(i => string.Equals(i.Path, path, StringComparison.OrdinalIgnoreCase)))
                    return;

                inputs.Add(new ExportInput
                {
                    Index = inputs.Count,
                    Kind = kind,
                    Path = path,
                    AudioTrack = audioTrack
                });
            }

            bool needsScreen = request.VideoLayout == RecordingExportVideoLayout.ScreenOnly ||
                               request.VideoLayout == RecordingExportVideoLayout.ScreenWebcamPictureInPicture ||
                               request.VideoLayout == RecordingExportVideoLayout.ScreenWebcamSideBySide;

            bool needsWebcam = request.VideoLayout == RecordingExportVideoLayout.WebcamOnly ||
                               request.VideoLayout == RecordingExportVideoLayout.ScreenWebcamPictureInPicture ||
                               request.VideoLayout == RecordingExportVideoLayout.ScreenWebcamSideBySide;

            if (needsScreen)
            {
                if (!info.HasScreen || info.ScreenPath == null)
                    return Fail("The selected export layout requires a screen track, but screen.mkv was not found.", request.OutputPath);

                AddInput("screen", info.ScreenPath);
                screenIndex = inputs.First(i => i.Kind == "screen").Index;
            }

            if (needsWebcam)
            {
                if (!info.HasWebcam || info.WebcamPath == null)
                    return Fail("The selected export layout requires a webcam track, but webcam.mp4 was not found.", request.OutputPath);

                AddInput("webcam", info.WebcamPath);
                webcamIndex = inputs.First(i => i.Kind == "webcam").Index;
            }

            var requestedAudio = ResolveSelectedAudioInputs(info, request.AudioMode);
            foreach (var audio in requestedAudio)
            {
                AddInput(audio.Kind, audio.Path, audio.AudioTrack);
            }

            var selectedAudio = inputs
                .Where(i => string.Equals(i.Kind, "mic", StringComparison.OrdinalIgnoreCase) ||
                            string.Equals(i.Kind, "system", StringComparison.OrdinalIgnoreCase))
                .ToList();

            micIndex = inputs.FirstOrDefault(i => i.Kind == "mic")?.Index ?? -1;
            systemIndex = inputs.FirstOrDefault(i => i.Kind == "system")?.Index ?? -1;

            if (!needsScreen && !needsWebcam && selectedAudio.Count == 0)
                return Fail("No valid audio/video tracks are available for this export.", request.OutputPath);

            var args = new List<string> { "-y", "-hide_banner" };

            foreach (var input in inputs)
            {
                args.Add("-i");
                args.Add(input.Path);
            }

            var filterParts = new List<string>();
            string? videoMap = null;
            string? audioMap = null;
            bool shouldReencodeVideo = false;
            int targetFps = ResolveOutputFps(info);

            if (request.VideoLayout == RecordingExportVideoLayout.ScreenOnly && screenIndex >= 0)
            {
                videoMap = $"{screenIndex}:v:0";
            }
            else if (request.VideoLayout == RecordingExportVideoLayout.WebcamOnly && webcamIndex >= 0)
            {
                videoMap = $"{webcamIndex}:v:0";
            }
            else if (request.VideoLayout == RecordingExportVideoLayout.ScreenWebcamPictureInPicture)
            {
                filterParts.Add($"[{screenIndex}:v]setpts=PTS-STARTPTS,fps={targetFps}[screenv]");
                filterParts.Add($"[{webcamIndex}:v]setpts=PTS-STARTPTS,fps={targetFps},scale=trunc(iw*0.25/2)*2:-2[cam]");
                filterParts.Add("[screenv][cam]overlay=W-w-24:H-h-24:shortest=1[v]");
                videoMap = "[v]";
                shouldReencodeVideo = true;
            }
            else if (request.VideoLayout == RecordingExportVideoLayout.ScreenWebcamSideBySide)
            {
                int targetHeight = ResolveEvenHeight(info);
                filterParts.Add($"[{screenIndex}:v]setpts=PTS-STARTPTS,fps={targetFps},scale=-2:{targetHeight}[screen]");
                filterParts.Add($"[{webcamIndex}:v]setpts=PTS-STARTPTS,fps={targetFps},scale=-2:{targetHeight}[cam]");
                filterParts.Add("[screen][cam]hstack=inputs=2:shortest=1[v]");
                videoMap = "[v]";
                shouldReencodeVideo = true;
            }

            if (selectedAudio.Count > 0)
            {
                audioMap = BuildAudioFilter(selectedAudio, filterParts);
            }

            if (filterParts.Count > 0)
            {
                args.Add("-filter_complex");
                args.Add(string.Join(";", filterParts));
            }

            if (!string.IsNullOrWhiteSpace(videoMap))
            {
                args.Add("-map");
                args.Add(videoMap);

                if (shouldReencodeVideo)
                {
                    var quality = ResolveQualityArgs(request.Quality);
                    args.Add("-c:v");
                    args.Add("libx264");
                    args.Add("-preset");
                    args.Add(quality.Preset);
                    args.Add("-crf");
                    args.Add(quality.Crf.ToString(CultureInfo.InvariantCulture));
                    args.Add("-pix_fmt");
                    args.Add("yuv420p");
                }
                else
                {
                    args.Add("-c:v");
                    args.Add("copy");
                }
            }
            else
            {
                args.Add("-vn");
            }

            if (!string.IsNullOrWhiteSpace(audioMap))
            {
                args.Add("-map");
                args.Add(audioMap);
                AddAudioCodecArgs(args, request.Container, request.Quality);
            }
            else
            {
                if (IsAudioOnlyContainer(request.Container))
                    return Fail("The selected audio-only container requires at least one audio track.", request.OutputPath);

                args.Add("-an");
            }

            var masterDuration = ResolveMasterExportDuration(info, request.VideoLayout);
            if (masterDuration.TotalSeconds > 0 && (!string.IsNullOrWhiteSpace(videoMap) || !string.IsNullOrWhiteSpace(audioMap)))
            {
                args.Add("-t");
                args.Add(FormatSeconds(masterDuration.TotalSeconds));
            }

            if (request.Container == RecordingExportContainer.Mp4)
            {
                args.Add("-movflags");
                args.Add("+faststart");
            }
            args.Add(request.OutputPath);

            var result = await RunFFmpegAsync(ffmpegPath, args, progress, cancellationToken);
            result.OutputPath = request.OutputPath;

            if (result.Success)
                result.Message = $"Export completed successfully.\n{request.OutputPath}";

            return result;
        }

        private static async Task<RecordingExportResult> ExportSeparateTracksAsync(
            RecordingExportSessionInfo info,
            RecordingExportRequest request,
            IProgress<string>? progress,
            CancellationToken cancellationToken)
        {
            var ffmpegPath = FindFFmpegPath();

            if (!File.Exists(ffmpegPath))
            {
                return new RecordingExportResult
                {
                    Success = false,
                    Message = $"FFmpeg was not found at:\n{ffmpegPath}",
                    OutputPath = request.OutputPath
                };
            }

            if (string.IsNullOrWhiteSpace(request.OutputPath))
                request.OutputPath = BuildDefaultSeparateTracksOutputFolder(info.ProjectFolder);

            Directory.CreateDirectory(request.OutputPath);

            var combinedLog = new StringBuilder();
            int completed = 0;

            async Task<bool> RunTrackExportAsync(string title, List<string> args)
            {
                progress?.Report(title);
                var result = await RunFFmpegAsync(ffmpegPath, args, progress, cancellationToken);
                combinedLog.AppendLine(result.Log);

                if (!result.Success)
                {
                    progress?.Report(result.Message);
                    return false;
                }

                completed++;
                return true;
            }

            if (request.ExportScreenTrack && info.HasScreen && info.ScreenPath != null)
            {
                var output = Path.Combine(request.OutputPath, "screen.mp4");
                if (!await RunTrackExportAsync("Exporting screen track...", new List<string>
                    {
                        "-y", "-hide_banner", "-i", info.ScreenPath,
                        "-map", "0:v:0", "-c:v", "copy", "-an", output
                    }))
                    return Fail("Unable to export the screen track.", request.OutputPath, combinedLog.ToString());
            }

            if (request.ExportWebcamTrack && info.HasWebcam && info.WebcamPath != null)
            {
                var output = Path.Combine(request.OutputPath, "webcam.mp4");
                if (!await RunTrackExportAsync("Exporting webcam track...", new List<string>
                    {
                        "-y", "-hide_banner", "-i", info.WebcamPath,
                        "-map", "0:v:0", "-c:v", "copy", "-an", output
                    }))
                    return Fail("Unable to export the webcam track.", request.OutputPath, combinedLog.ToString());
            }

            if (request.ExportAudioMixTrack)
            {
                var selectedAudio = ResolveSelectedAudioInputs(info, request.AudioMode);

                if (selectedAudio.Count > 0)
                {
                    var inputs = new List<ExportInput>();
                    foreach (var audio in selectedAudio)
                    {
                        inputs.Add(new ExportInput
                        {
                            Index = inputs.Count,
                            Kind = audio.Kind,
                            Path = audio.Path,
                            AudioTrack = audio.AudioTrack
                        });
                    }

                    var args = new List<string> { "-y", "-hide_banner" };
                    foreach (var input in inputs)
                    {
                        args.Add("-i");
                        args.Add(input.Path);
                    }

                    var filterParts = new List<string>();
                    var audioMap = BuildAudioFilter(inputs, filterParts);
                    if (filterParts.Count > 0)
                    {
                        args.Add("-filter_complex");
                        args.Add(string.Join(";", filterParts));
                    }

                    args.Add("-map");
                    args.Add(audioMap);
                    args.Add("-c:a");
                    args.Add("pcm_s16le");
                    args.Add(Path.Combine(request.OutputPath, "audio_mix.wav"));

                    if (!await RunTrackExportAsync("Exporting audio mix...", args))
                        return Fail("Unable to export the audio mix.", request.OutputPath, combinedLog.ToString());
                }
            }

            if (completed == 0)
                return Fail("No valid tracks were selected for separate export.", request.OutputPath, combinedLog.ToString());

            return new RecordingExportResult
            {
                Success = true,
                Message = $"Separate tracks export completed successfully.\n{request.OutputPath}",
                OutputPath = request.OutputPath,
                Log = combinedLog.ToString()
            };
        }

        private static List<ExportInput> ResolveSelectedAudioInputs(RecordingExportSessionInfo info, RecordingExportAudioMode audioMode)
        {
            var result = new List<ExportInput>();

            if ((audioMode == RecordingExportAudioMode.MicrophoneOnly || audioMode == RecordingExportAudioMode.MicrophoneAndSystemAudio) &&
                info.HasMic && info.MicPath != null)
            {
                result.Add(new ExportInput
                {
                    Kind = "mic",
                    Path = info.MicPath,
                    AudioTrack = info.Manifest.Mic
                });
            }

            if ((audioMode == RecordingExportAudioMode.SystemAudioOnly || audioMode == RecordingExportAudioMode.MicrophoneAndSystemAudio) &&
                info.HasSystemAudio && info.SystemPath != null)
            {
                result.Add(new ExportInput
                {
                    Kind = "system",
                    Path = info.SystemPath,
                    AudioTrack = info.Manifest.System
                });
            }

            for (int i = 0; i < result.Count; i++)
                result[i].Index = i;

            return result;
        }

        private static string BuildAudioFilter(List<ExportInput> selectedAudio, List<string> filterParts)
        {
            var labels = new List<string>();

            for (int i = 0; i < selectedAudio.Count; i++)
            {
                var input = selectedAudio[i];
                var label = $"a{i}";
                var offsetMs = Math.Max(0, input.AudioTrack?.OffsetMs ?? 0);
                var channels = Math.Max(1, input.AudioTrack?.Channels ?? 2);
                var operations = new List<string>();

                operations.Add("asetpts=PTS-STARTPTS");

                if (offsetMs > 0)
                {
                    var delays = string.Join("|", Enumerable.Repeat(offsetMs.ToString(CultureInfo.InvariantCulture), channels));
                    operations.Add($"adelay={delays}");
                }
                else
                {
                    operations.Add("anull");
                }

                filterParts.Add($"[{input.Index}:a]{string.Join(",", operations)}[{label}]");
                labels.Add($"[{label}]");
            }

            if (labels.Count == 1)
            {
                filterParts.Add($"{labels[0]}anull[a]");
                return "[a]";
            }

            filterParts.Add($"{string.Join(string.Empty, labels)}amix=inputs={labels.Count}:duration=longest:normalize=0[a]");
            return "[a]";
        }

        private static TimeSpan ResolveMasterExportDuration(RecordingExportSessionInfo info, RecordingExportVideoLayout videoLayout)
        {
            if (info.Manifest?.DurationMs != null && info.Manifest.DurationMs.Value > 0)
                return TimeSpan.FromMilliseconds(info.Manifest.DurationMs.Value);

            var videoMasterDuration = videoLayout switch
            {
                RecordingExportVideoLayout.ScreenOnly => info.Manifest.Screen?.Duration,
                RecordingExportVideoLayout.ScreenWebcamPictureInPicture => info.Manifest.Screen?.Duration,
                RecordingExportVideoLayout.ScreenWebcamSideBySide => info.Manifest.Screen?.Duration,
                RecordingExportVideoLayout.WebcamOnly => info.Manifest.Webcam?.Duration,
                _ => null
            };

            if (TryParseManifestDuration(videoMasterDuration, out var parsed) && parsed.TotalSeconds > 0)
                return parsed;

            return TimeSpan.Zero;
        }

        private static int ResolveOutputFps(RecordingExportSessionInfo info)
        {
            int fps = 30;

            if (info.Manifest.Screen?.Fps > 0)
                fps = info.Manifest.Screen.Fps;
            else if (info.Manifest.Webcam?.Fps > 0)
                fps = info.Manifest.Webcam.Fps;

            if (fps < 1)
                fps = 30;

            if (fps > 120)
                fps = 120;

            return fps;
        }

        private static string BuildLeadingTrimFilter(double trimSeconds)
        {
            if (trimSeconds <= 0)
                return string.Empty;

            return $"trim=start={FormatSeconds(trimSeconds)},";
        }

        private static double ResolveLeadingTrimSeconds(string? trackDuration, string? referenceDuration)
        {
            if (!TryParseManifestDuration(trackDuration, out var track) ||
                !TryParseManifestDuration(referenceDuration, out var reference))
                return 0;

            var diff = track.TotalSeconds - reference.TotalSeconds;

            if (diff < 0.75)
                return 0;

            return Math.Round(diff, 3);
        }

        private static bool TryParseManifestDuration(string? value, out TimeSpan duration)
        {
            duration = TimeSpan.Zero;

            if (string.IsNullOrWhiteSpace(value))
                return false;

            return TimeSpan.TryParse(value, CultureInfo.InvariantCulture, out duration);
        }

        private static string FormatSeconds(double value)
        {
            return value.ToString("0.###", CultureInfo.InvariantCulture);
        }

        private static async Task<RecordingExportResult> RunFFmpegAsync(
            string ffmpegPath,
            List<string> args,
            IProgress<string>? progress,
            CancellationToken cancellationToken)
        {
            var log = new StringBuilder();
            var displayCommand = BuildDisplayCommand(ffmpegPath, args);
            DebugLog.Write("[ExportService] FFmpeg command:\n" + displayCommand);
            progress?.Report(displayCommand);

            var psi = new ProcessStartInfo
            {
                FileName = ffmpegPath,
                UseShellExecute = false,
                CreateNoWindow = true,
                RedirectStandardError = true,
                RedirectStandardOutput = true
            };

            foreach (var arg in args)
                psi.ArgumentList.Add(arg);

            using var process = new Process { StartInfo = psi, EnableRaisingEvents = true };

            process.OutputDataReceived += (_, e) =>
            {
                if (string.IsNullOrWhiteSpace(e.Data))
                    return;

                log.AppendLine(e.Data);
                DebugLog.Write("[ExportService][stdout] " + e.Data);
                progress?.Report(e.Data);
            };

            process.ErrorDataReceived += (_, e) =>
            {
                if (string.IsNullOrWhiteSpace(e.Data))
                    return;

                log.AppendLine(e.Data);
                DebugLog.Write("[ExportService][stderr] " + e.Data);

                if (e.Data.Contains("time=", StringComparison.OrdinalIgnoreCase) ||
                    e.Data.Contains("error", StringComparison.OrdinalIgnoreCase) ||
                    e.Data.Contains("failed", StringComparison.OrdinalIgnoreCase))
                {
                    progress?.Report(e.Data);
                }
            };

            try
            {
                process.Start();
                process.BeginOutputReadLine();
                process.BeginErrorReadLine();
                await process.WaitForExitAsync(cancellationToken);
            }
            catch (OperationCanceledException)
            {
                try
                {
                    if (!process.HasExited)
                        process.Kill(entireProcessTree: true);
                }
                catch { }

                return new RecordingExportResult
                {
                    Success = false,
                    Message = "Export cancelled.",
                    Log = log.ToString()
                };
            }
            catch (Exception ex)
            {
                DebugLog.Write("[ExportService] FFmpeg process ERROR:\n" + ex);
                return new RecordingExportResult
                {
                    Success = false,
                    Message = "Unable to start FFmpeg.\n" + ex.Message,
                    Log = log.ToString()
                };
            }

            if (process.ExitCode != 0)
            {
                return new RecordingExportResult
                {
                    Success = false,
                    Message = $"FFmpeg failed with exit code {process.ExitCode}.",
                    Log = log.ToString()
                };
            }

            return new RecordingExportResult
            {
                Success = true,
                Message = "FFmpeg finished successfully.",
                Log = log.ToString()
            };
        }

        private static string ResolveManifestPath(string projectFolder)
        {
            var candidates = new[]
            {
                Path.Combine(projectFolder, "manifest.json"),
                Path.Combine(projectFolder, "session.manifest.json")
            };

            foreach (var candidate in candidates)
            {
                if (File.Exists(candidate))
                    return candidate;
            }

            throw new FileNotFoundException("No manifest.json or session.manifest.json was found in this recording folder.");
        }

        private static string? ResolveExistingTrackPath(string projectFolder, string? fileName)
        {
            if (string.IsNullOrWhiteSpace(fileName))
                return null;

            var path = Path.IsPathRooted(fileName)
                ? fileName
                : Path.Combine(projectFolder, fileName);

            return File.Exists(path) ? path : null;
        }

        private static int ResolveEvenHeight(RecordingExportSessionInfo info)
        {
            int height = 720;

            if (info.Manifest.Screen?.Height > 0)
                height = info.Manifest.Screen.Height;
            else if (info.Manifest.Webcam?.Height > 0)
                height = info.Manifest.Webcam.Height;

            if (height % 2 != 0)
                height--;

            return Math.Max(2, height);
        }

        private static bool IsAudioOnlyContainer(RecordingExportContainer container)
        {
            return container == RecordingExportContainer.Mp3 ||
                   container == RecordingExportContainer.Wav;
        }

        private static void AddAudioCodecArgs(List<string> args, RecordingExportContainer container, RecordingExportQuality quality)
        {
            if (container == RecordingExportContainer.Mp3)
            {
                args.Add("-c:a");
                args.Add("libmp3lame");
                args.Add("-b:a");
                args.Add(ResolveAudioBitrate(quality));
                return;
            }

            if (container == RecordingExportContainer.Wav)
            {
                var wav = ResolveWavQuality(quality);
                args.Add("-c:a");
                args.Add(wav.Codec);
                args.Add("-ar");
                args.Add(wav.SampleRate.ToString(CultureInfo.InvariantCulture));
                return;
            }

            args.Add("-c:a");
            args.Add("aac");
            args.Add("-b:a");
            args.Add(ResolveAudioBitrate(quality));
        }

        private static string ResolveAudioBitrate(RecordingExportQuality quality)
        {
            return quality switch
            {
                RecordingExportQuality.Audio128Kbps => "128k",
                RecordingExportQuality.Audio320Kbps => "320k",
                _ => "192k"
            };
        }

        private static (string Codec, int SampleRate) ResolveWavQuality(RecordingExportQuality quality)
        {
            return quality switch
            {
                RecordingExportQuality.Wav44100Hz16Bit => ("pcm_s16le", 44100),
                RecordingExportQuality.Wav48000Hz24Bit => ("pcm_s24le", 48000),
                _ => ("pcm_s16le", 48000)
            };
        }

        private static (string Preset, int Crf) ResolveQualityArgs(RecordingExportQuality quality)
        {
            return quality switch
            {
                RecordingExportQuality.Fast => ("veryfast", 26),
                RecordingExportQuality.HighQuality => ("slow", 18),
                _ => ("medium", 23)
            };
        }

        private static RecordingExportResult Fail(string message, string outputPath, string log = "")
        {
            return new RecordingExportResult
            {
                Success = false,
                Message = message,
                OutputPath = outputPath,
                Log = log
            };
        }

        private static string SanitizeFileName(string raw)
        {
            if (string.IsNullOrWhiteSpace(raw))
                return "recording";

            var invalid = Path.GetInvalidFileNameChars();
            var clean = new string(raw.Select(ch => invalid.Contains(ch) ? '_' : ch).ToArray());
            return string.IsNullOrWhiteSpace(clean) ? "recording" : clean.Trim();
        }

        private static string BuildDisplayCommand(string ffmpegPath, IEnumerable<string> args)
        {
            return QuoteArg(ffmpegPath) + " " + string.Join(" ", args.Select(QuoteArg));
        }

        private static string QuoteArg(string arg)
        {
            if (string.IsNullOrWhiteSpace(arg))
                return "\"\"";

            if (arg.Contains(' ') || arg.Contains('"') || arg.Contains(';') || arg.Contains('[') || arg.Contains(']'))
                return "\"" + arg.Replace("\"", "\\\"") + "\"";

            return arg;
        }
    }
}

using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Text;
using System.Threading.Tasks;
namespace NessStudio.Models
{
    public sealed class RecordingOutputPaths
    {
        public string BaseDir { get; }
        public string ScreenPrefix { get; } = "screen";
        public string WebcamPrefix { get; } = "webcam";
        public string MicPrefix { get; } = "mic";
        public string SystemPrefix { get; } = "system";
        public string ScreenExt { get; } = ".mkv";
        public string WebcamExt { get; } = ".mp4";
        public string MicExt { get; } = ".wav";
        public string SystemExt { get; } = ".wav";
        public RecordingOutputPaths(string baseDir)
        {
            if (string.IsNullOrWhiteSpace(baseDir))
                throw new ArgumentException("Base directory is required.", nameof(baseDir));
            BaseDir = baseDir;
            Directory.CreateDirectory(BaseDir);
        }
        public string SegmentFile(string prefix, int segment, string ext)
            => Path.Combine(BaseDir, $"{prefix}_{segment:00}{ext}");
        public string ScreenSegment(int segment) => SegmentFile(ScreenPrefix, segment, ScreenExt);

        public string ScreenContinuous() => Path.Combine(BaseDir, $"{ScreenPrefix}{ScreenExt}");
        public string WebcamContinuous() => Path.Combine(BaseDir, $"{WebcamPrefix}{WebcamExt}");

        public string MicContinuous() => Path.Combine(BaseDir, $"{MicPrefix}{MicExt}");
        public string SystemContinuous() => Path.Combine(BaseDir, $"{SystemPrefix}{SystemExt}");

        public string WebcamSegment(int segment) => SegmentFile(WebcamPrefix, segment, WebcamExt);
        public string MicSegment(int segment) => SegmentFile(MicPrefix, segment, MicExt);
        public string SystemSegment(int segment) => SegmentFile(SystemPrefix, segment, SystemExt);
        public List<string> Parts(string prefix, string ext)
        {
            var pattern = $"{prefix}_*{ext}";
            return Directory
                .GetFiles(BaseDir, pattern)
                .OrderBy(f => f, StringComparer.OrdinalIgnoreCase)
                .ToList();
        }
    }
}
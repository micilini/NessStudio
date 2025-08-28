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

        public string ScreenExt { get; } = ".mp4";
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
        public string WebcamSegment(int segment) => SegmentFile(WebcamPrefix, segment, WebcamExt);
        public string MicSegment(int segment) => SegmentFile(MicPrefix, segment, MicExt);
        public string SystemSegment(int segment) => SegmentFile(SystemPrefix, segment, SystemExt);

        public string FinalFile(string prefix, string ext)
            => Path.Combine(BaseDir, $"{prefix}_final{ext}");

        public string ConcatListFile(string prefix)
            => Path.Combine(BaseDir, $"concat_{prefix}.txt");

        public string ScreenFinal() => FinalFile(ScreenPrefix, ScreenExt);
        public string WebcamFinal() => FinalFile(WebcamPrefix, WebcamExt);
        public string MicFinal() => FinalFile(MicPrefix, MicExt);
        public string SystemFinal() => FinalFile(SystemPrefix, SystemExt);

        public List<string> Parts(string prefix, string ext)
        {
            var pattern = $"{prefix}_*{ext}";
            return Directory
                .GetFiles(BaseDir, pattern)
                .OrderBy(f => f, StringComparer.OrdinalIgnoreCase)
                .ToList();
        }

        public void WriteConcatList(string prefix, IEnumerable<string> files)
        {
            var listPath = ConcatListFile(prefix);
            using var sw = new StreamWriter(listPath, false, new UTF8Encoding(encoderShouldEmitUTF8Identifier: false));
            foreach (var p in files)
            {
                var esc = p.Replace("'", "''");
                sw.WriteLine($"file '{esc}'");
            }
        }
    }
}

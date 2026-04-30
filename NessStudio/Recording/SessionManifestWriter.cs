using System;
using System.IO;
using System.Text.Json;
using NAudio.Wave;
namespace NessStudio.Recording
{
    public static class SessionManifestWriter
    {
        public static string WriteJson(string baseDir, SessionManifest manifest)
        {
            Directory.CreateDirectory(baseDir);

            var opts = new JsonSerializerOptions
            {
                WriteIndented = true
            };

            string json = JsonSerializer.Serialize(manifest, opts);

            
            string primaryPath = Path.Combine(baseDir, "manifest.json");

            
            string legacyPath = Path.Combine(baseDir, "session.manifest.json");

            File.WriteAllText(primaryPath, json);
            File.WriteAllText(legacyPath, json);

            return primaryPath;
        }
        public static SessionManifest.AudioTrack ReadAudioInfo(string wavPath)
        {
            if (string.IsNullOrWhiteSpace(wavPath) || !File.Exists(wavPath))
                return null;
            using var r = new WaveFileReader(wavPath);
            return new SessionManifest.AudioTrack
            {
                File = Path.GetFileName(wavPath),
                Duration = FormatDuration(r.TotalTime),
                SampleRate = r.WaveFormat.SampleRate,
                Channels = r.WaveFormat.Channels,
                BitsPerSample = r.WaveFormat.BitsPerSample
            };
        }
        public static SessionManifest.SegmentEntry BuildAudioSegment(string wavPath)
        {
            if (string.IsNullOrWhiteSpace(wavPath) || !File.Exists(wavPath))
                return null;
            using var r = new WaveFileReader(wavPath);
            return new SessionManifest.SegmentEntry
            {
                File = Path.GetFileName(wavPath),
                Duration = FormatDuration(r.TotalTime)
            };
        }
        public static string SumDurations(IEnumerable<SessionManifest.SegmentEntry> segments)
        {
            if (segments == null)
                return "00:00:00";
            TimeSpan total = TimeSpan.Zero;
            foreach (var seg in segments)
            {
                if (seg == null || string.IsNullOrWhiteSpace(seg.Duration))
                    continue;
                if (TimeSpan.TryParse(seg.Duration, out var ts))
                    total += ts;
            }
            return FormatDuration(total);
        }
        public static string FormatDuration(TimeSpan value)
        {
            if (value < TimeSpan.Zero)
                value = TimeSpan.Zero;
            int hours = (int)value.TotalHours;
            return $"{hours:00}:{value.Minutes:00}:{value.Seconds:00}";
        }
    }
}
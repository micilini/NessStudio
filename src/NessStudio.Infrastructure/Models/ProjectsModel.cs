using System;
using SQLite;
using System.Globalization;
namespace NessStudio.Models
{
    [Table("Projects")]
    public class ProjectsModel
    {
        [PrimaryKey, AutoIncrement]
        public int Id { get; set; }
        [Indexed(Unique = true)]
        public string ProjectId { get; set; } = Guid.NewGuid().ToString("N");
        [Indexed]
        public string Title { get; set; } = "Untitled Recording";
        /// <summary>Shows automatic recording names as a local date and time while retaining custom titles.</summary>
        [Ignore]
        public string DisplayTitle
        {
            get
            {
                string title = Title;
                if (string.IsNullOrWhiteSpace(title)) return "Untitled Recording";
                ReadOnlySpan<char> timestamp = title.AsSpan();
                string format = "yyyyMMdd_HHmmss";
                if (title.Length == 26 && title[19] == '_' &&
                    timestamp[20..].IndexOfAnyExcept("0123456789abcdefABCDEF") < 0)
                {
                    timestamp = timestamp[..19];
                    format = "yyyyMMdd_HHmmss_fff";
                }
                else if (title.Length == 19) format = "yyyy_MM_dd HH:mm:ss";
                else if (title.Length != 15) return title;

                return DateTime.TryParseExact(timestamp, format, CultureInfo.InvariantCulture, DateTimeStyles.None, out var recordedAt)
                    ? recordedAt.ToString("MM/dd/yyyy HH:mm:ss", CultureInfo.InvariantCulture)
                    : title;
            }
        }
        public string ThumbnailPath { get; set; } = string.Empty;
        public string ProjectFolderPath { get; set; } = string.Empty;
        public string ProjectFilePath { get; set; } = string.Empty;
        public long FileSizeBytes { get; set; } = 0;
        [Indexed]
        public DateTime CreatedAt { get; set; } = DateTime.Now;
        [Indexed]
        public DateTime UpdatedAt { get; set; } = DateTime.Now;
        [Indexed]
        public DateTime LastOpenedAt { get; set; } = DateTime.Now;
        public bool IsFavorite { get; set; } = false;
        public int VideoWidth { get; set; } = 0;
        public int VideoHeight { get; set; } = 0;
        public double FrameRate { get; set; } = 0.0;
        public long DurationMs { get; set; } = 0;
        public bool HasMicrophoneAudio { get; set; } = false;
        public bool HasSystemAudio { get; set; } = false;
        public bool HasWebcam { get; set; } = false;
        public double AppVersionAtCreation { get; set; } = 1.0;
        public double AppVersionAtLastSave { get; set; } = 1.0;
        public string Tags { get; set; } = string.Empty;
        [Indexed]
        public bool IsDeleted { get; set; } = false;
        [Ignore]
        public string FileSizeHuman =>
            FileSizeBytes <= 0 ? "0 B" : ToHumanSize(FileSizeBytes);
        [Ignore]
        public string ResolutionDisplay =>
            (VideoWidth > 0 && VideoHeight > 0) ? $"{VideoWidth}×{VideoHeight}" : "—";
        [Ignore]
        public string DurationDisplay =>
            DurationMs > 0 ? TimeSpan.FromMilliseconds(DurationMs).ToString(@"hh\:mm\:ss") : "—";
        private static string ToHumanSize(long bytes)
        {
            string[] units = { "B", "KB", "MB", "GB", "TB" };
            double size = bytes;
            int unit = 0;
            while (size >= 1024 && unit < units.Length - 1)
            {
                size /= 1024;
                unit++;
            }
            return $"{size:0.##} {units[unit]}";
        }
    }
}
using System;
namespace NessStudio.Models
{
    public class RecordingPreferencesModel
    {
        public int TimerSeconds { get; set; } = 3;
        public int RecordingFps { get; set; } = 30;
        public DateTime CreatedAtUtc { get; set; } = DateTime.UtcNow;
        public DateTime UpdatedAtUtc { get; set; } = DateTime.UtcNow;
    }
}
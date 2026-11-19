using System;
namespace NessStudio.Models
{
    /// <summary>Preserves the recording preference values and JSON property names.</summary>
    public class RecordingPreferencesModel
    {
        /// <summary>Gets or sets the selected countdown in seconds.</summary>
        public int TimerSeconds { get; set; } = 3;
        /// <summary>Gets or sets the selected output frame rate.</summary>
        public int RecordingFps { get; set; } = 30;
        /// <summary>Gets or sets the recording execution policy; absent preferences retain Automatic.</summary>
        public NessStudio.Recording.Engines.RecordingExecutionMode ExecutionMode { get; set; }
        /// <summary>Gets or sets the recording resolution and compression preset.</summary>
        public NessStudio.Recording.Engines.RecordingQualityPreset QualityPreset { get; set; } = NessStudio.Recording.Engines.RecordingQualityPreset.High;
        /// <summary>Gets or sets the application's microphone recording level (0–100 percent).</summary>
        public double MicrophoneLevel { get; set; } = 100;
        /// <summary>Gets or sets the application's loopback recording level (0–100 percent).</summary>
        public double SystemAudioLevel { get; set; } = 100;
        /// <summary>Gets or sets the preference creation time.</summary>
        public DateTime CreatedAtUtc { get; set; } = DateTime.UtcNow;
        /// <summary>Gets or sets the last persisted update time.</summary>
        public DateTime UpdatedAtUtc { get; set; } = DateTime.UtcNow;
    }
}

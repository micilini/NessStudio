namespace NessStudio.Models
{
    /// <summary>Describes the existing legacy recording presets independently of UI.</summary>
    public class RecordingRuntimeOptions
    {
        /// <summary>Gets or sets the selected integer frame rate.</summary>
        public int RecordingFps { get; set; } = 30;
        /// <summary>Gets or sets the frontend countdown in seconds.</summary>
        public int CountdownSeconds { get; set; } = 3;

        /// <summary>Gets or sets the existing screen warmup limit in milliseconds.</summary>
        public int ScreenWarmupMilliseconds { get; set; } = 1200;
        /// <summary>Gets or sets the existing camera warmup limit in milliseconds.</summary>
        public int WebcamWarmupMilliseconds { get; set; } = 900;
        /// <summary>Gets or sets the thumbnail sample offset in seconds.</summary>
        public double ThumbnailCaptureOffsetSeconds { get; set; } = 2.0;
    }
}

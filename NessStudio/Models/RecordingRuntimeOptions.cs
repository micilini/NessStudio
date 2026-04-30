namespace NessStudio.Models
{
    public class RecordingRuntimeOptions
    {
        public int RecordingFps { get; set; } = 30;
        public int CountdownSeconds { get; set; } = 3;

        public int ScreenWarmupMilliseconds { get; set; } = 1200;
        public int WebcamWarmupMilliseconds { get; set; } = 900;
        public double ThumbnailCaptureOffsetSeconds { get; set; } = 2.0;
    }
}
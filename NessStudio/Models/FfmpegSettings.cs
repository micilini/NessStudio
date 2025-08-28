using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Text;
using System.Threading.Tasks;

namespace NessStudio.Models
{
    public sealed class FfmpegSettings
    {
        public string FfmpegExePath { get; init; }

        public int Framerate { get; init; } = 30;
        public string VideoCodec { get; init; } = "libx264";
        public string Preset { get; init; } = "veryfast";
        public string PixelFormat { get; init; } = "yuv420p";
        public bool Cfr { get; init; } = true;
        public bool FastStart { get; init; } = true;
        public bool UseWallclockAsTimestamps { get; init; } = true;

        public int CrfScreen { get; init; } = 18;
        public int CrfWebcam { get; init; } = 20;

        public int ProbeSizeMb { get; init; } = 100;
        public int ThreadQueueSizeScreen { get; init; } = 512;
        public int ThreadQueueSizeWebcam { get; init; } = 1024;
        public bool DrawMouse { get; init; } = true;
        public string RtbufsizeWebcam { get; init; } = "512M";

        public static FfmpegSettings CreateDefault()
        {
            return new FfmpegSettings
            {
                FfmpegExePath = Path.Combine(AppDomain.CurrentDomain.BaseDirectory, "Modules", "ffmpeg.exe")
            };
        }
    }
}

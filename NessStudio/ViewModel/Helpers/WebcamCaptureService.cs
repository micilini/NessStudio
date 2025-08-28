using NessStudio.Models;
using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.Linq;
using System.Text;
using System.Threading.Tasks;

namespace NessStudio.ViewModel.Helpers
{
    public static class WebcamCaptureService
    {
        public static Process Start(FfmpegSettings s, string webcamName, string outputPath)
        {
            if (string.IsNullOrWhiteSpace(webcamName)) return null;
            var args = FfmpegArgsBuilder.BuildWebcamArgs(s, webcamName, outputPath);
            return FfmpegProcessService.Start(s.FfmpegExePath, args);
        }

        public static void Stop(ref Process p)
        {
            FfmpegProcessService.TryStop(ref p);
        }
    }
}

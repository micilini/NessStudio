using NessStudio.Models;
using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.Linq;
using System.Text;
using System.Threading.Tasks;

namespace NessStudio.ViewModel.Helpers
{
    public static class ScreenCaptureService
    {
        public static Process Start(
            FfmpegSettings s,
            ScreenRegion region,
            RecordingOutputPaths paths,
            int segment)
        {
            if (s == null || region == null || paths == null) return null;

            if (!ScreenRegionService.ComputeGdiArgs(region, out int x, out int y, out int w, out int h, out var _))
                return null;

            string outFile = paths.ScreenSegment(segment);
            var args = FfmpegArgsBuilder.BuildScreenArgs(s, x, y, w, h, outFile);
            return FfmpegProcessService.Start(s.FfmpegExePath, args);
        }

        public static void Stop(ref Process p)
        {
            FfmpegProcessService.TryStop(ref p);
        }
    }
}

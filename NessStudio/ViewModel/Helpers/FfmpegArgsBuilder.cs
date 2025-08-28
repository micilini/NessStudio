using NessStudio.Models;
using System;
using System.Collections.Generic;
using System.Linq;
using System.Text;
using System.Threading.Tasks;

namespace NessStudio.ViewModel.Helpers
{
    public static class FfmpegArgsBuilder
    {
        public static string BuildScreenArgs(FfmpegSettings s, int x, int y, int w, int h, string outputPath)
        {
            if (s is null) throw new ArgumentNullException(nameof(s));
            if (string.IsNullOrWhiteSpace(outputPath)) throw new ArgumentException("Output path required.", nameof(outputPath));

            var sb = new StringBuilder();
            sb.Append("-y ");
            sb.AppendFormat("-f gdigrab -probesize {0}M -framerate {1} -thread_queue_size {2} ", s.ProbeSizeMb, s.Framerate, s.ThreadQueueSizeScreen);
            sb.AppendFormat("-offset_x {0} -offset_y {1} -video_size {2}x{3} ", x, y, w, h);
            if (s.DrawMouse) sb.Append("-draw_mouse 1 ");
            sb.Append("-i desktop ");
            if (s.UseWallclockAsTimestamps) sb.Append("-use_wallclock_as_timestamps 1 ");
            sb.Append("-fflags +genpts ");
            if (s.Cfr) sb.Append("-vsync cfr ");
            sb.AppendFormat("-r {0} ", s.Framerate);
            sb.AppendFormat("-c:v {0} -preset {1} -crf {2} -pix_fmt {3} ", s.VideoCodec, s.Preset, s.CrfScreen, s.PixelFormat);
            if (s.FastStart) sb.Append("-movflags +faststart ");
            sb.AppendFormat("\"{0}\"", outputPath);
            return sb.ToString();
        }

        public static string BuildWebcamArgs(FfmpegSettings s, string webcamName, string outputPath)
        {
            if (s is null) throw new ArgumentNullException(nameof(s));
            if (string.IsNullOrWhiteSpace(webcamName)) throw new ArgumentException("Webcam name required.", nameof(webcamName));
            if (string.IsNullOrWhiteSpace(outputPath)) throw new ArgumentException("Output path required.", nameof(outputPath));

            var sb = new StringBuilder();
            sb.Append("-y ");
            sb.AppendFormat("-f dshow -rtbufsize {0} -thread_queue_size {1} ", s.RtbufsizeWebcam, s.ThreadQueueSizeWebcam);
            sb.AppendFormat("-i video=\"{0}\" ", webcamName);
            sb.AppendFormat("-r {0} ", s.Framerate);
            sb.AppendFormat("-c:v {0} -preset {1} -crf {2} -pix_fmt {3} ", s.VideoCodec, s.Preset, s.CrfWebcam, s.PixelFormat);
            if (s.FastStart) sb.Append("-movflags +faststart ");
            sb.AppendFormat("\"{0}\"", outputPath);
            return sb.ToString();
        }
    }
}

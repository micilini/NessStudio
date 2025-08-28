using NessStudio.Models;
using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Text;
using System.Threading.Tasks;

namespace NessStudio.ViewModel.Helpers
{
    public static class TrackFinalizer
    {
        public static List<string> ListParts(string baseDir, string prefix, string ext)
        {
            if (string.IsNullOrWhiteSpace(baseDir)) return new List<string>();
            var paths = new RecordingOutputPaths(baseDir);
            return paths.Parts(prefix, ext).ToList();
        }

        public static ConcatPlan BuildPlan(RecordingOutputPaths paths, string prefix, string ext)
        {
            if (paths == null) throw new ArgumentNullException(nameof(paths));
            return ConcatPlan.Build(paths, prefix, ext);
        }

        public static async Task<string> ConcatIfNeededAsync(string ffmpegExePath, ConcatPlan plan)
        {
            if (plan == null) throw new ArgumentNullException(nameof(plan));
            if (plan.Parts == null || plan.Parts.Count == 0)
                return null;

            if (!plan.NeedsConcat)
                return plan.Parts[0];

            plan.WriteListFile();

            var args = $"-y -f concat -safe 0 -i \"{plan.ListTxtPath}\" -c copy \"{plan.OutputPath}\"";
            await FfmpegProcessService.RunOnceAsync(ffmpegExePath, args);

            if (File.Exists(plan.OutputPath))
            {
                try { File.Delete(plan.ListTxtPath); } catch { }
                foreach (var p in plan.Parts)
                    try { File.Delete(p); } catch { }

                return plan.OutputPath;
            }

            try { File.Delete(plan.ListTxtPath); } catch { }
            return plan.Parts[0];
        }
    }
}

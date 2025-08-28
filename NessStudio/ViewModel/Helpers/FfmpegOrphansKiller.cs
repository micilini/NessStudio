using NessStudio.Models;
using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.IO;
using System.Linq;
using System.Text;
using System.Threading.Tasks;

namespace NessStudio.ViewModel.Helpers
{
    public static class FfmpegOrphansKiller
    {
        public static int KillIfMatchesAppModules()
        {
            int killed = 0;

            var settings = FfmpegSettings.CreateDefault();
            var mainDir = SafeGetDir(settings.FfmpegExePath);

            var legacyDir = SafeCombine(AppDomain.CurrentDomain.BaseDirectory, "tools", "ffmpeg");

            var candidateDirs = new HashSet<string>(StringComparer.OrdinalIgnoreCase);
            if (!string.IsNullOrWhiteSpace(mainDir)) candidateDirs.Add(mainDir);
            if (!string.IsNullOrWhiteSpace(legacyDir)) candidateDirs.Add(legacyDir);

            foreach (var proc in Process.GetProcessesByName("ffmpeg"))
            {
                try
                {
                    string exe = proc.MainModule?.FileName;
                    if (string.IsNullOrWhiteSpace(exe)) continue;

                    var dir = Path.GetDirectoryName(exe);
                    if (dir == null) continue;

                    if (candidateDirs.Contains(dir))
                    {
                        try
                        {
                            proc.Kill(entireProcessTree: true);
                            if (!proc.WaitForExit(2000))
                                proc.Kill();
                            killed++;
                        }
                        catch {  }
                    }
                }
                catch
                {
                    
                }
                finally
                {
                    try { proc.Dispose(); } catch { }
                }
            }

            return killed;

            static string SafeGetDir(string path)
            {
                try { return string.IsNullOrWhiteSpace(path) ? null : Path.GetDirectoryName(path); }
                catch { return null; }
            }

            static string SafeCombine(string a, string b, string c)
            {
                try { return Path.Combine(a, b, c); } catch { return null; }
            }
        }
    }
}

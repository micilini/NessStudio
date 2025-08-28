using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.IO;
using System.Linq;
using System.Text;
using System.Threading.Tasks;

namespace NessStudio.ViewModel.Helpers
{
    public static class FfmpegProcessService
    {
        public static Process Start(string ffmpegPath, string args, string extraLoglevel = " -loglevel info")
        {
            if (string.IsNullOrWhiteSpace(ffmpegPath) || !File.Exists(ffmpegPath))
                throw new FileNotFoundException($"ffmpeg.exe not found at: {ffmpegPath}");

            var psi = new ProcessStartInfo
            {
                FileName = ffmpegPath,
                Arguments = (args ?? string.Empty) + (extraLoglevel ?? string.Empty),
                UseShellExecute = false,
                CreateNoWindow = true,
                RedirectStandardInput = true,
                RedirectStandardError = false,
                RedirectStandardOutput = false
            };

            return Process.Start(psi);
        }

        public static void TryStop(ref Process p, int gracefulMs = 8000, int killMs = 2000)
        {
            if (p == null) return;
            try
            {
                if (!p.HasExited)
                {
                    try
                    {
                        // graceful
                        p.StandardInput.WriteLine("q");
                        if (!p.WaitForExit(gracefulMs))
                        {
                            p.Kill();
                            p.WaitForExit(killMs);
                        }
                    }
                    catch
                    {
                        try { p.Kill(); p.WaitForExit(killMs); } catch { }
                    }
                }
            }
            finally
            {
                try { p.Dispose(); } catch { }
                p = null;
            }
        }

        public static async Task<int> RunOnceAsync(string ffmpegPath, string args, string extraLoglevel = " -loglevel info")
        {
            using var p = Start(ffmpegPath, args, extraLoglevel);
            await Task.Run(() => p.WaitForExit());
            return p.ExitCode;
        }
    }
}

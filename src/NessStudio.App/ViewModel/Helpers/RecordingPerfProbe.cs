using System;
using System.Diagnostics;
using System.Threading;

namespace NessStudio.ViewModel.Helpers
{
    public static class RecordingPerfProbe
    {
        private static long _sequence;

        public static void Mark(string stage, string details = null)
        {
            var snapshot = CaptureSnapshot();

            DebugLog.Write(
                $"[PERF] #{Interlocked.Increment(ref _sequence)} " +
                $"stage={stage} | " +
                $"ws={snapshot.WorkingSetMb:F1}MB | " +
                $"private={snapshot.PrivateMb:F1}MB | " +
                $"managed={snapshot.ManagedMb:F1}MB | " +
                $"gen0={snapshot.Gen0} | gen1={snapshot.Gen1} | gen2={snapshot.Gen2}" +
                FormatDetails(details));
        }

        public static IDisposable Scope(string name, string details = null)
        {
            return new PerfScope(name, details);
        }

        private static Snapshot CaptureSnapshot()
        {
            try
            {
                using Process current = Process.GetCurrentProcess();

                return new Snapshot(
                    current.WorkingSet64 / 1024d / 1024d,
                    current.PrivateMemorySize64 / 1024d / 1024d,
                    GC.GetTotalMemory(false) / 1024d / 1024d,
                    GC.CollectionCount(0),
                    GC.CollectionCount(1),
                    GC.CollectionCount(2));
            }
            catch
            {
                return new Snapshot(-1d, -1d, -1d, -1, -1, -1);
            }
        }

        private static string FormatDetails(string details)
        {
            return string.IsNullOrWhiteSpace(details) ? string.Empty : $" | {details}";
        }

        private readonly struct Snapshot
        {
            public Snapshot(double workingSetMb, double privateMb, double managedMb, int gen0, int gen1, int gen2)
            {
                WorkingSetMb = workingSetMb;
                PrivateMb = privateMb;
                ManagedMb = managedMb;
                Gen0 = gen0;
                Gen1 = gen1;
                Gen2 = gen2;
            }

            public double WorkingSetMb { get; }
            public double PrivateMb { get; }
            public double ManagedMb { get; }
            public int Gen0 { get; }
            public int Gen1 { get; }
            public int Gen2 { get; }
        }

        private sealed class PerfScope : IDisposable
        {
            private readonly string _name;
            private readonly string _details;
            private readonly Stopwatch _sw;
            private readonly Snapshot _start;
            private bool _disposed;

            public PerfScope(string name, string details)
            {
                _name = name;
                _details = details;
                _start = CaptureSnapshot();
                _sw = Stopwatch.StartNew();

                DebugLog.Write(
                    $"[PERF] #{Interlocked.Increment(ref _sequence)} " +
                    $"scope-begin={_name} | " +
                    $"ws={_start.WorkingSetMb:F1}MB | " +
                    $"private={_start.PrivateMb:F1}MB | " +
                    $"managed={_start.ManagedMb:F1}MB | " +
                    $"gen0={_start.Gen0} | gen1={_start.Gen1} | gen2={_start.Gen2}" +
                    FormatDetails(_details));
            }

            public void Dispose()
            {
                if (_disposed)
                    return;

                _disposed = true;
                _sw.Stop();

                var end = CaptureSnapshot();

                DebugLog.Write(
                    $"[PERF] #{Interlocked.Increment(ref _sequence)} " +
                    $"scope-end={_name} | " +
                    $"elapsed={_sw.Elapsed.TotalMilliseconds:F0}ms | " +
                    $"ws={end.WorkingSetMb:F1}MB (Δ{end.WorkingSetMb - _start.WorkingSetMb:+0.0;-0.0;0.0}MB) | " +
                    $"private={end.PrivateMb:F1}MB (Δ{end.PrivateMb - _start.PrivateMb:+0.0;-0.0;0.0}MB) | " +
                    $"managed={end.ManagedMb:F1}MB (Δ{end.ManagedMb - _start.ManagedMb:+0.0;-0.0;0.0}MB) | " +
                    $"gen0={end.Gen0 - _start.Gen0:+#;-#;0} | " +
                    $"gen1={end.Gen1 - _start.Gen1:+#;-#;0} | " +
                    $"gen2={end.Gen2 - _start.Gen2:+#;-#;0}" +
                    FormatDetails(_details));
            }
        }
    }
}
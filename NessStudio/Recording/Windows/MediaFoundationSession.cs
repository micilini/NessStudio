using System;
using System.Runtime.InteropServices;
using NessStudio.ViewModel.Helpers;

namespace NessStudio.Recording.Windows
{
    internal static class MediaFoundationSession
    {
        private const int MF_VERSION = 0x00020070;
        private const int MFSTARTUP_FULL = 0;

        private static readonly object _sync = new object();
        private static int _refCount;

        public static void Acquire()
        {
            lock (_sync)
            {
                if (_refCount == 0)
                {
                    int hr = MFStartup(MF_VERSION, MFSTARTUP_FULL);
                    if (hr < 0)
                        Marshal.ThrowExceptionForHR(hr);

                    DebugLog.Write("[MFSession] MFStartup OK");
                }
                else
                {
                    DebugLog.Write($"[MFSession] Acquire reuse | refCount={_refCount} -> {_refCount + 1}");
                }

                _refCount++;
            }
        }

        public static void Release()
        {
            lock (_sync)
            {
                if (_refCount <= 0)
                {
                    DebugLog.Write("[MFSession] Release called but refCount already 0");
                    return;
                }

                _refCount--;
                DebugLog.Write($"[MFSession] Release | refCount now={_refCount}");

                if (_refCount == 0)
                {
                    try
                    {
                        int hr = MFShutdown();
                        if (hr < 0)
                            DebugLog.Write($"[MFSession] MFShutdown returned hr=0x{hr:X8}");
                        else
                            DebugLog.Write("[MFSession] MFShutdown OK");
                    }
                    catch (Exception ex)
                    {
                        DebugLog.Write("[MFSession] MFShutdown ERROR:\n" + ex);
                    }
                }
            }
        }

        [DllImport("mfplat.dll", ExactSpelling = true)]
        private static extern int MFStartup(int version, int dwFlags);

        [DllImport("mfplat.dll", ExactSpelling = true)]
        private static extern int MFShutdown();
    }
}
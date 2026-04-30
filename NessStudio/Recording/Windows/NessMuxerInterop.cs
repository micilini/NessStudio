using System;
using System.IO;
using System.Runtime.InteropServices;

namespace NessStudio.Recording.Windows
{
    
    
    
    
    internal static class NessMuxerInterop
    {
        private const string DLL = "NessMuxer";

        static NessMuxerInterop()
        {
            try
            {
                string baseDir = AppContext.BaseDirectory;

                string rootDll = Path.Combine(baseDir, "NessMuxer.dll");
                string nestedDll = Path.Combine(baseDir, "Native", "NessMuxer", "NessMuxer.dll");

                if (File.Exists(rootDll))
                {
                    NativeLibrary.Load(rootDll);
                    ViewModel.Helpers.DebugLog.Write($"[NessMuxerInterop] NativeLibrary.Load OK => {rootDll}");
                    return;
                }

                if (File.Exists(nestedDll))
                {
                    NativeLibrary.Load(nestedDll);
                    ViewModel.Helpers.DebugLog.Write($"[NessMuxerInterop] NativeLibrary.Load OK => {nestedDll}");
                    return;
                }

                ViewModel.Helpers.DebugLog.Write(
                    $"[NessMuxerInterop] DLL not found. Tried:\n - {rootDll}\n - {nestedDll}");
            }
            catch (Exception ex)
            {
                ViewModel.Helpers.DebugLog.Write("[NessMuxerInterop] NativeLibrary.Load ERROR:\n" + ex);
                throw;
            }
        }

        

        public const int NESS_OK = 0;
        public const int NESS_ERROR = -1;
        public const int NESS_ERROR_IO = -2;
        public const int NESS_ERROR_PARAM = -3;
        public const int NESS_ERROR_STATE = -4;
        public const int NESS_ERROR_ENCODER = -5;
        public const int NESS_ERROR_ALLOC = -6;

        

        public const int NESS_ENCODER_AUTO = 0;
        public const int NESS_ENCODER_MEDIA_FOUNDATION = 1;
        public const int NESS_ENCODER_X264 = 2;
        public const int NESS_ENCODER_NVENC = 3;
        public const int NESS_ENCODER_VIDEOTOOLBOX = 4;

        
        
        
        
        [StructLayout(LayoutKind.Sequential, CharSet = CharSet.Ansi)]
        public struct NessMuxerConfig
        {
            [MarshalAs(UnmanagedType.LPStr)]
            public string output_path;

            public int width;
            public int height;
            public int fps;
            public int bitrate_kbps;
            public int encoder_type;
        }

        
        
        
        
        [DllImport(DLL, CallingConvention = CallingConvention.Cdecl)]
        public static extern int ness_muxer_open(out IntPtr muxer, ref NessMuxerConfig config);

        
        
        
        
        [DllImport(DLL, CallingConvention = CallingConvention.Cdecl)]
        public static extern int ness_muxer_write_frame(IntPtr muxer, IntPtr nv12_data, int nv12_size);

        
        
        
        
        [DllImport(DLL, CallingConvention = CallingConvention.Cdecl)]
        public static extern int ness_muxer_close(IntPtr muxer);

        
        
        
        [DllImport(DLL, CallingConvention = CallingConvention.Cdecl)]
        public static extern IntPtr ness_muxer_error(IntPtr muxer);

        
        
        
        [DllImport(DLL, CallingConvention = CallingConvention.Cdecl)]
        public static extern long ness_muxer_frame_count(IntPtr muxer);

        
        
        
        [DllImport(DLL, CallingConvention = CallingConvention.Cdecl)]
        public static extern long ness_muxer_encoded_count(IntPtr muxer);

        
        
        
        public static string GetError(IntPtr muxer)
        {
            IntPtr ptr = ness_muxer_error(muxer);
            return ptr != IntPtr.Zero ? Marshal.PtrToStringAnsi(ptr) ?? "" : "";
        }
    }
}
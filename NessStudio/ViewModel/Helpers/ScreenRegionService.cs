using NessStudio.Models;
using System;
using System.Collections.Generic;
using System.Linq;
using System.Runtime.InteropServices;
using System.Text;
using System.Threading.Tasks;

namespace NessStudio.ViewModel.Helpers
{
    public static class ScreenRegionService
    {
        public static bool ComputeGdiArgs(
            ScreenRegion region,
            out int offsetX, out int offsetY, out int width, out int height,
            out string displayName)
        {
            offsetX = offsetY = width = height = 0;
            displayName = null;
            if (region == null || !region.ShouldCapture) return false;

            int vX = GetSystemMetrics(SM_XVIRTUALSCREEN);
            int vY = GetSystemMetrics(SM_YVIRTUALSCREEN);
            int vW = GetSystemMetrics(SM_CXVIRTUALSCREEN);
            int vH = GetSystemMetrics(SM_CYVIRTUALSCREEN);

            if (region.CropGdi.HasValue)
            {
                var r = region.CropGdi.Value;
                offsetX = r.X - vX;
                offsetY = r.Y - vY;
                width = r.Width;
                height = r.Height;
                ClampToVirtual(vW, vH, ref offsetX, ref offsetY, ref width, ref height);
                MakeEven(ref width, ref height);
                return true;
            }

            if (region.SelectedScreen == null)
                return false;

            var mid = new System.Drawing.Point(
                region.SelectedScreen.Bounds.Left + region.SelectedScreen.Bounds.Width / 2,
                region.SelectedScreen.Bounds.Top + region.SelectedScreen.Bounds.Height / 2
            );
            IntPtr hmon = MonitorFromPoint(mid, 2);

            var mi = new MONITORINFOEX { cbSize = Marshal.SizeOf<MONITORINFOEX>() };
            if (!GetMonitorInfo(hmon, ref mi))
                return false;

            displayName = mi.szDevice;

            int rx = mi.rcMonitor.Left;
            int ry = mi.rcMonitor.Top;
            int rw = mi.rcMonitor.Right - mi.rcMonitor.Left;
            int rh = mi.rcMonitor.Bottom - mi.rcMonitor.Top;

            double scale = 1.0;
            if (!string.IsNullOrWhiteSpace(mi.szDevice)
                && TryGetPhysicalResolution(mi.szDevice, out int physW, out int physH)
                && physW > 0 && physH > 0)
            {
                double sx = (double)rw / physW;
                double sy = (double)rh / physH;
                scale = Math.Max(sx, sy);
                if (scale < 1.0) scale = 1.0;
            }

            offsetX = (int)Math.Round((rx - vX) / scale);
            offsetY = (int)Math.Round((ry - vY) / scale);
            width = (int)Math.Round(rw / scale);
            height = (int)Math.Round(rh / scale);

            ClampToVirtual(vW, vH, ref offsetX, ref offsetY, ref width, ref height);
            MakeEven(ref width, ref height);
            return true;
        }

        public static bool TryGetPhysicalResolution(string display, out int w, out int h)
        {
            var dm = new DEVMODE { dmSize = (short)Marshal.SizeOf(typeof(DEVMODE)) };
            bool ok = EnumDisplaySettings(display, ENUM_CURRENT_SETTINGS, ref dm);
            w = ok ? dm.dmPelsWidth : 0;
            h = ok ? dm.dmPelsHeight : 0;
            return ok && w > 0 && h > 0;
        }

        private static void ClampToVirtual(int vW, int vH, ref int x, ref int y, ref int w, ref int h)
        {
            if (x < 0) x = 0;
            if (y < 0) y = 0;
            if (w < 16) w = 16;
            if (h < 16) h = 16;
            if (x + w > vW) w = Math.Max(16, vW - x);
            if (y + h > vH) h = Math.Max(16, vH - y);
        }

        private static void MakeEven(ref int w, ref int h)
        {
            if ((w & 1) == 1) w--;
            if ((h & 1) == 1) h--;
            if (w < 16) w = 16;
            if (h < 16) h = 16;
        }

        [DllImport("user32.dll")] private static extern IntPtr MonitorFromPoint(System.Drawing.Point pt, uint dwFlags);
        [DllImport("user32.dll")] private static extern int GetSystemMetrics(int nIndex);
        [DllImport("user32.dll", CharSet = CharSet.Auto)]
        private static extern bool GetMonitorInfo(IntPtr hMonitor, ref MONITORINFOEX lpmi);
        [DllImport("user32.dll", CharSet = CharSet.Auto)]
        private static extern bool EnumDisplaySettings(string lpszDeviceName, int iModeNum, ref DEVMODE lpDevMode);

        private const int SM_XVIRTUALSCREEN = 76;
        private const int SM_YVIRTUALSCREEN = 77;
        private const int SM_CXVIRTUALSCREEN = 78;
        private const int SM_CYVIRTUALSCREEN = 79;
        private const int ENUM_CURRENT_SETTINGS = -1;

        [StructLayout(LayoutKind.Sequential)] private struct RECT { public int Left, Top, Right, Bottom; }

        [StructLayout(LayoutKind.Sequential, CharSet = CharSet.Auto)]
        private struct MONITORINFOEX
        {
            public int cbSize;
            public RECT rcMonitor;
            public RECT rcWork;
            public int dwFlags;
            [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 32)]
            public string szDevice;
        }

        [StructLayout(LayoutKind.Sequential, CharSet = CharSet.Auto)]
        private struct DEVMODE
        {
            private const int CCHDEVICENAME = 32, CCHFORMNAME = 32;
            [MarshalAs(UnmanagedType.ByValTStr, SizeConst = CCHDEVICENAME)]
            public string dmDeviceName;
            public short dmSpecVersion, dmDriverVersion, dmSize, dmDriverExtra;
            public int dmFields, dmPositionX, dmPositionY, dmDisplayOrientation, dmDisplayFixedOutput;
            public short dmColor, dmDuplex, dmYResolution, dmTTOption, dmCollate;
            [MarshalAs(UnmanagedType.ByValTStr, SizeConst = CCHFORMNAME)]
            public string dmFormName;
            public short dmLogPixels;
            public int dmBitsPerPel, dmPelsWidth, dmPelsHeight, dmDisplayFlags, dmDisplayFrequency;
            public int dmICMMethod, dmICMIntent, dmMediaType, dmDitherType, dmReserved1, dmReserved2;
            public int dmPanningWidth, dmPanningHeight;
        }
    }
}

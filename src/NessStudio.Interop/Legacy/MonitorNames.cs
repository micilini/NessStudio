using System.Runtime.InteropServices;

namespace NessStudio.Interop.Legacy;

/// <summary>Resolves legacy monitor labels without exposing native structures to ViewModels.</summary>
public static class MonitorNames
{
        [StructLayout(LayoutKind.Sequential, CharSet = CharSet.Auto)]
        private struct DISPLAY_DEVICE
        {
            public int cb;
            [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 32)]
            public string DeviceName;
            [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 128)]
            public string DeviceString;
            public int StateFlags;
            [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 128)]
            public string DeviceID;
            [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 128)]
            public string DeviceKey;
        }
        [DllImport("user32.dll", CharSet = CharSet.Auto)]
        private static extern bool EnumDisplayDevices(string lpDevice, int iDevNum, ref DISPLAY_DEVICE lpDisplayDevice, int dwFlags);
        public static string GetFriendlyName(string deviceName)
        {
            var dd = new DISPLAY_DEVICE { cb = Marshal.SizeOf<DISPLAY_DEVICE>() };
            int i = 0;
            while (EnumDisplayDevices(null, i, ref dd, 0))
            {
                if (string.Equals(dd.DeviceName, deviceName, StringComparison.OrdinalIgnoreCase))
                {
                    var ddMon = new DISPLAY_DEVICE { cb = Marshal.SizeOf<DISPLAY_DEVICE>() };
                    if (EnumDisplayDevices(dd.DeviceName, 0, ref ddMon, 0) && !string.IsNullOrWhiteSpace(ddMon.DeviceString))
                        return ddMon.DeviceString;
                    return string.IsNullOrWhiteSpace(dd.DeviceString) ? deviceName : dd.DeviceString;
                }
                dd = new DISPLAY_DEVICE { cb = Marshal.SizeOf<DISPLAY_DEVICE>() };
                i++;
            }
            return deviceName;
        }
}

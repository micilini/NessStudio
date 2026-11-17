using NessStudio.ViewModel;
using System;
using System.Collections.Generic;
using System.Linq;
using System.Runtime.InteropServices;
using System.Text;
using System.Threading.Tasks;
using System.Windows;
using System.Windows.Controls;
using System.Windows.Data;
using System.Windows.Documents;
using System.Windows.Input;
using System.Windows.Interop;
using System.Windows.Media;
using System.Windows.Media.Imaging;
using System.Windows.Shapes;
namespace NessStudio.View.HomeScreen
{
    public partial class HomeScreenWindow : Window
    {
        public HomeScreenWindow()
        {
            InitializeComponent();
            var vm = new HomeScreenWindowVM(this);
            DataContext = vm;
            Loaded += FitInitialWorkArea;
            this.SourceInitialized += (s, e) =>
            {
                var src = (HwndSource)PresentationSource.FromVisual(this);
                if (src != null)
                    src.AddHook(WndProc);
            };
        }
        private void FitInitialWorkArea(object sender, RoutedEventArgs e)
        {
            Loaded -= FitInitialWorkArea;
            if (WindowState != WindowState.Normal ||
                PresentationSource.FromVisual(this) is not HwndSource source ||
                source.CompositionTarget is null)
                return;

            IntPtr monitor = MonitorFromWindow(source.Handle, MONITOR_DEFAULTTONEAREST);
            MONITORINFO info = new() { cbSize = Marshal.SizeOf<MONITORINFO>() };
            if (monitor == IntPtr.Zero || !GetMonitorInfo(monitor, ref info))
                return;

            var transform = source.CompositionTarget.TransformFromDevice;
            var available = transform.Transform(new Vector(
                info.rcWork.right - info.rcWork.left, info.rcWork.bottom - info.rcWork.top));
            if (available.X <= 0 || available.Y <= 0)
                return;

            // Convert distances from this window's pixel origin, not absolute
            // virtual-screen coordinates, which can span monitors with different DPI.
            var origin = PointToScreen(new Point());
            var offset = transform.Transform(new Vector(info.rcWork.left - origin.X, info.rcWork.top - origin.Y));
            double workLeft = Left + offset.X, workTop = Top + offset.Y;
            double initialLeft = Left, initialTop = Top;
            MinWidth = Math.Min(MinWidth, available.X);
            MinHeight = Math.Min(MinHeight, available.Y);
            Width = Math.Min(Width, available.X);
            Height = Math.Min(Height, available.Y);
            Left = Math.Clamp(initialLeft, workLeft, workLeft + available.X - Width);
            Top = Math.Clamp(initialTop, workTop, workTop + available.Y - Height);
        }
        private IntPtr WndProc(IntPtr hwnd, int msg, IntPtr wParam, IntPtr lParam, ref bool handled)
        {
            const int WM_GETMINMAXINFO = 0x0024;
            if (msg == WM_GETMINMAXINFO)
            {
                WmGetMinMaxInfo(hwnd, lParam);
                handled = true;
            }
            return IntPtr.Zero;
        }
        private void WmGetMinMaxInfo(IntPtr hwnd, IntPtr lParam)
        {
            IntPtr monitor = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
            if (monitor == IntPtr.Zero) return;
            MONITORINFO monitorInfo = new MONITORINFO();
            monitorInfo.cbSize = Marshal.SizeOf(typeof(MONITORINFO));
            if (!GetMonitorInfo(monitor, ref monitorInfo)) return;
            RECT work = monitorInfo.rcWork;
            RECT monitorArea = monitorInfo.rcMonitor;
            MINMAXINFO mmi = Marshal.PtrToStructure<MINMAXINFO>(lParam);
            mmi.ptMaxPosition.x = Math.Abs(work.left - monitorArea.left);
            mmi.ptMaxPosition.y = Math.Abs(work.top - monitorArea.top);
            mmi.ptMaxSize.x = Math.Abs(work.right - work.left);
            mmi.ptMaxSize.y = Math.Abs(work.bottom - work.top);
            mmi.ptMaxTrackSize.x = mmi.ptMaxSize.x;
            mmi.ptMaxTrackSize.y = mmi.ptMaxSize.y;
            var src = (HwndSource)PresentationSource.FromVisual(this);
            if (src?.CompositionTarget != null)
            {
                var m = src.CompositionTarget.TransformToDevice;
                int minTrackX = (int)Math.Ceiling(this.MinWidth * m.M11);
                int minTrackY = (int)Math.Ceiling(this.MinHeight * m.M22);
                if (minTrackX <= 0) minTrackX = 1;
                if (minTrackY <= 0) minTrackY = 1;
                mmi.ptMinTrackSize.x = Math.Min(minTrackX, mmi.ptMaxTrackSize.x);
                mmi.ptMinTrackSize.y = Math.Min(minTrackY, mmi.ptMaxTrackSize.y);
            }
            Marshal.StructureToPtr(mmi, lParam, true);
        }
        private const int MONITOR_DEFAULTTONEAREST = 0x00000002;
        [DllImport("user32.dll")]
        private static extern IntPtr MonitorFromWindow(IntPtr hwnd, int dwFlags);
        [DllImport("user32.dll", EntryPoint = "GetMonitorInfoW", ExactSpelling = true)]
        [return: MarshalAs(UnmanagedType.Bool)]
        private static extern bool GetMonitorInfo(IntPtr hMonitor, ref MONITORINFO lpmi);
        [StructLayout(LayoutKind.Sequential)]
        private struct MONITORINFO
        {
            public int cbSize;
            public RECT rcMonitor;
            public RECT rcWork;
            public int dwFlags;
        }
        [StructLayout(LayoutKind.Sequential)]
        private struct RECT
        {
            public int left;
            public int top;
            public int right;
            public int bottom;
        }
        [StructLayout(LayoutKind.Sequential)]
        private struct MINMAXINFO
        {
            public POINT ptReserved;
            public POINT ptMaxSize;
            public POINT ptMaxPosition;
            public POINT ptMinTrackSize;
            public POINT ptMaxTrackSize;
        }
        [StructLayout(LayoutKind.Sequential)]
        private struct POINT
        {
            public int x;
            public int y;
        }
    }
}
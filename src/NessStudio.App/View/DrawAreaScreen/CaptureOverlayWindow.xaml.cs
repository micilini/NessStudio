using NessStudio.ViewModel.Helpers;
using System;
using System.Runtime.InteropServices;
using System.Windows;
using System.Windows.Controls;
using System.Windows.Interop;

namespace NessStudio.View.DrawAreaScreen
{
    public partial class CaptureOverlayWindow : Window
    {
        private const int SM_XVIRTUALSCREEN = 76;
        private const int SM_YVIRTUALSCREEN = 77;
        private const int SM_CXVIRTUALSCREEN = 78;
        private const int SM_CYVIRTUALSCREEN = 79;

        private const int GWL_EXSTYLE = -20;

        private const long WS_EX_TRANSPARENT = 0x00000020L;
        private const long WS_EX_TOOLWINDOW = 0x00000080L;
        private const long WS_EX_APPWINDOW = 0x00040000L;

        private const uint MONITOR_DEFAULTTOPRIMARY = 1;
        private const uint WDA_EXCLUDEFROMCAPTURE = 0x00000011;

        private readonly Rect _cropScreenPx;

        public CaptureOverlayWindow(Rect cropScreenPx)
        {
            _cropScreenPx = Normalize(cropScreenPx);

            InitializeComponent();

            SizeChanged += (_, __) => Redraw();
            LocationChanged += (_, __) => Redraw();
        }

        protected override void OnSourceInitialized(EventArgs e)
        {
            base.OnSourceInitialized(e);

            ApplyVirtualScreenBounds();
            ApplyWindowStylesAndCaptureExclusion();
        }

        private void Window_Loaded(object sender, RoutedEventArgs e)
        {
            ApplyVirtualScreenBounds();
            UpdateLayout();
            Redraw();
        }

        private void ApplyVirtualScreenBounds()
        {
            int gdiX = GetSystemMetrics(SM_XVIRTUALSCREEN);
            int gdiY = GetSystemMetrics(SM_YVIRTUALSCREEN);
            int gdiW = GetSystemMetrics(SM_CXVIRTUALSCREEN);
            int gdiH = GetSystemMetrics(SM_CYVIRTUALSCREEN);

            double dpiX = 1.0;
            double dpiY = 1.0;

            try
            {
                IntPtr primaryMonitor = MonitorFromPoint(new POINT(0, 0), MONITOR_DEFAULTTOPRIMARY);

                uint rawDpiX;
                uint rawDpiY;

                if (GetDpiForMonitor(primaryMonitor, 0, out rawDpiX, out rawDpiY) == 0)
                {
                    dpiX = rawDpiX / 96.0;
                    dpiY = rawDpiY / 96.0;
                }
            }
            catch
            {
                dpiX = 1.0;
                dpiY = 1.0;
            }

            WindowState = WindowState.Normal;
            WindowStartupLocation = WindowStartupLocation.Manual;

            Left = gdiX / dpiX;
            Top = gdiY / dpiY;
            Width = gdiW / dpiX;
            Height = gdiH / dpiY;
        }

        private void ApplyWindowStylesAndCaptureExclusion()
        {
            try
            {
                var hwnd = new WindowInteropHelper(this).Handle;

                if (hwnd == IntPtr.Zero)
                    return;

                long exStyle = GetWindowLongPtrSafe(hwnd, GWL_EXSTYLE).ToInt64();

                exStyle |= WS_EX_TOOLWINDOW;
                exStyle |= WS_EX_TRANSPARENT;
                exStyle &= ~WS_EX_APPWINDOW;

                SetWindowLongPtrSafe(hwnd, GWL_EXSTYLE, new IntPtr(exStyle));

                bool affinityOk = false;

                try
                {
                    affinityOk = SetWindowDisplayAffinity(hwnd, WDA_EXCLUDEFROMCAPTURE);
                }
                catch
                {
                    affinityOk = false;
                }

                DebugLog.Write($"[CaptureOverlay] initialized | hwnd={hwnd} | excludeFromCapture={affinityOk}");
            }
            catch (Exception ex)
            {
                DebugLog.Write("[CaptureOverlay] ApplyWindowStylesAndCaptureExclusion ERROR:\n" + ex);
            }
        }

        private void Redraw()
        {
            try
            {
                double canvasWidth = OverlayCanvas.ActualWidth > 0 ? OverlayCanvas.ActualWidth : ActualWidth;
                double canvasHeight = OverlayCanvas.ActualHeight > 0 ? OverlayCanvas.ActualHeight : ActualHeight;

                if (canvasWidth <= 0 || canvasHeight <= 0)
                    return;

                var cropDip = GetCropDipRect();
                var bounds = new Rect(0, 0, canvasWidth, canvasHeight);

                cropDip.Intersect(bounds);

                if (cropDip.IsEmpty || cropDip.Width <= 0 || cropDip.Height <= 0)
                {
                    SetElement(TopShade, 0, 0, canvasWidth, canvasHeight);
                    SetElement(BottomShade, 0, 0, 0, 0);
                    SetElement(LeftShade, 0, 0, 0, 0);
                    SetElement(RightShade, 0, 0, 0, 0);
                    SetElement(CropBorder, 0, 0, 0, 0);
                    CaptureBadge.Visibility = Visibility.Collapsed;
                    return;
                }

                CaptureBadge.Visibility = Visibility.Visible;

                SetElement(TopShade, 0, 0, canvasWidth, cropDip.Top);
                SetElement(BottomShade, 0, cropDip.Bottom, canvasWidth, Math.Max(0, canvasHeight - cropDip.Bottom));
                SetElement(LeftShade, 0, cropDip.Top, cropDip.Left, cropDip.Height);
                SetElement(RightShade, cropDip.Right, cropDip.Top, Math.Max(0, canvasWidth - cropDip.Right), cropDip.Height);

                SetElement(CropBorder, cropDip.Left, cropDip.Top, cropDip.Width, cropDip.Height);

                PositionBadge(cropDip, canvasWidth, canvasHeight);
            }
            catch (Exception ex)
            {
                DebugLog.Write("[CaptureOverlay] Redraw ERROR:\n" + ex);
            }
        }

        private Rect GetCropDipRect()
        {
            try
            {
                var topLeft = PointFromScreen(new Point(_cropScreenPx.Left, _cropScreenPx.Top));
                var bottomRight = PointFromScreen(new Point(_cropScreenPx.Right, _cropScreenPx.Bottom));

                return new Rect(topLeft, bottomRight);
            }
            catch
            {
                return Rect.Empty;
            }
        }

        private void PositionBadge(Rect cropDip, double canvasWidth, double canvasHeight)
        {
            if (double.IsNaN(CaptureBadge.ActualWidth) ||
                CaptureBadge.ActualWidth <= 0 ||
                double.IsNaN(CaptureBadge.ActualHeight) ||
                CaptureBadge.ActualHeight <= 0)
            {
                CaptureBadge.Measure(new Size(double.PositiveInfinity, double.PositiveInfinity));
                CaptureBadge.Arrange(new Rect(new Size(CaptureBadge.DesiredSize.Width, CaptureBadge.DesiredSize.Height)));
            }

            double badgeWidth = CaptureBadge.ActualWidth > 0 ? CaptureBadge.ActualWidth : CaptureBadge.DesiredSize.Width;
            double badgeHeight = CaptureBadge.ActualHeight > 0 ? CaptureBadge.ActualHeight : CaptureBadge.DesiredSize.Height;

            double x = cropDip.Left + ((cropDip.Width - badgeWidth) / 2.0);
            double y = cropDip.Top - badgeHeight - 10;

            if (y < 8)
                y = cropDip.Top + 10;

            x = Clamp(x, 8, Math.Max(8, canvasWidth - badgeWidth - 8));
            y = Clamp(y, 8, Math.Max(8, canvasHeight - badgeHeight - 8));

            Canvas.SetLeft(CaptureBadge, x);
            Canvas.SetTop(CaptureBadge, y);
        }

        private static void SetElement(FrameworkElement element, double left, double top, double width, double height)
        {
            Canvas.SetLeft(element, left);
            Canvas.SetTop(element, top);

            element.Width = Math.Max(0, width);
            element.Height = Math.Max(0, height);
        }

        private static Rect Normalize(Rect rect)
        {
            double left = Math.Min(rect.Left, rect.Right);
            double top = Math.Min(rect.Top, rect.Bottom);
            double right = Math.Max(rect.Left, rect.Right);
            double bottom = Math.Max(rect.Top, rect.Bottom);

            return new Rect(left, top, Math.Max(0, right - left), Math.Max(0, bottom - top));
        }

        private static double Clamp(double value, double min, double max)
        {
            if (value < min)
                return min;

            if (value > max)
                return max;

            return value;
        }

        private static IntPtr GetWindowLongPtrSafe(IntPtr hWnd, int nIndex)
        {
            if (IntPtr.Size == 8)
                return GetWindowLongPtr64(hWnd, nIndex);

            return new IntPtr(GetWindowLong32(hWnd, nIndex));
        }

        private static IntPtr SetWindowLongPtrSafe(IntPtr hWnd, int nIndex, IntPtr dwNewLong)
        {
            if (IntPtr.Size == 8)
                return SetWindowLongPtr64(hWnd, nIndex, dwNewLong);

            return new IntPtr(SetWindowLong32(hWnd, nIndex, dwNewLong.ToInt32()));
        }

        [DllImport("user32.dll")]
        private static extern int GetSystemMetrics(int nIndex);

        [DllImport("user32.dll")]
        private static extern IntPtr MonitorFromPoint(POINT pt, uint dwFlags);

        [DllImport("Shcore.dll")]
        private static extern int GetDpiForMonitor(IntPtr hmonitor, int dpiType, out uint dpiX, out uint dpiY);

        [DllImport("user32.dll")]
        private static extern bool SetWindowDisplayAffinity(IntPtr hwnd, uint dwAffinity);

        [DllImport("user32.dll", EntryPoint = "GetWindowLong")]
        private static extern int GetWindowLong32(IntPtr hWnd, int nIndex);

        [DllImport("user32.dll", EntryPoint = "GetWindowLongPtr")]
        private static extern IntPtr GetWindowLongPtr64(IntPtr hWnd, int nIndex);

        [DllImport("user32.dll", EntryPoint = "SetWindowLong")]
        private static extern int SetWindowLong32(IntPtr hWnd, int nIndex, int dwNewLong);

        [DllImport("user32.dll", EntryPoint = "SetWindowLongPtr")]
        private static extern IntPtr SetWindowLongPtr64(IntPtr hWnd, int nIndex, IntPtr dwNewLong);

        private struct POINT
        {
            public int X;
            public int Y;

            public POINT(int x, int y)
            {
                X = x;
                Y = y;
            }
        }
    }
}
using System;
using System.Collections.Generic;
using System.Linq;
using System.Runtime.InteropServices;
using System.Text;
using System.Threading.Tasks;
using System.Windows;
using System.Windows.Controls;
using System.Windows.Controls.Primitives;
using System.Windows.Data;
using System.Windows.Documents;
using System.Windows.Input;
using System.Windows.Media;
using System.Windows.Media.Imaging;
using System.Windows.Media.Media3D;
using System.Windows.Shapes;
using System.Runtime.InteropServices;
using System.Windows.Interop;

namespace NessStudio.View.DrawAreaScreen
{
    public partial class DrawAreaScreenWindow : Window
    {
        [DllImport("user32.dll")] static extern IntPtr MonitorFromPoint(POINT pt, uint dwFlags);
        [DllImport("Shcore.dll")] static extern int GetDpiForMonitor(IntPtr hmonitor, int dpiType, out uint dpiX, out uint dpiY);
        [DllImport("user32.dll")] static extern int GetSystemMetrics(int nIndex);

        const uint MONITOR_DEFAULTTOPRIMARY = 1;
        struct POINT { public int X, Y; public POINT(int x, int y) { X = x; Y = y; } }

        private void HandleNW_DragDelta(object sender, DragDeltaEventArgs e) => ResizeByEdges(left: e.HorizontalChange, top: e.VerticalChange);
        private void HandleN_DragDelta(object sender, DragDeltaEventArgs e) => ResizeByEdges(top: e.VerticalChange);
        private void HandleNE_DragDelta(object sender, DragDeltaEventArgs e) => ResizeByEdges(right: e.HorizontalChange, top: e.VerticalChange);
        private void HandleE_DragDelta(object sender, DragDeltaEventArgs e) => ResizeByEdges(right: e.HorizontalChange);
        private void HandleSE_DragDelta(object sender, DragDeltaEventArgs e) => ResizeByEdges(right: e.HorizontalChange, bottom: e.VerticalChange);
        private void HandleS_DragDelta(object sender, DragDeltaEventArgs e) => ResizeByEdges(bottom: e.VerticalChange);
        private void HandleSW_DragDelta(object sender, DragDeltaEventArgs e) => ResizeByEdges(left: e.HorizontalChange, bottom: e.VerticalChange);
        private void HandleW_DragDelta(object sender, DragDeltaEventArgs e) => ResizeByEdges(left: e.HorizontalChange);

        protected override void OnSourceInitialized(EventArgs e)
        {
            base.OnSourceInitialized(e);
            IntPtr hmonPrimary = MonitorFromPoint(new POINT(0, 0), MONITOR_DEFAULTTOPRIMARY);

            uint mdpiX = 96, mdpiY = 96;
            try { GetDpiForMonitor(hmonPrimary, 0, out mdpiX, out mdpiY); } catch {  }
            double dx = mdpiX / 96.0;
            double dy = mdpiY / 96.0;

            int gdiX = GetSystemMetrics(76);
            int gdiY = GetSystemMetrics(77);
            int gdiW = GetSystemMetrics(78);
            int gdiH = GetSystemMetrics(79);

            WindowState = WindowState.Normal;
            WindowStartupLocation = WindowStartupLocation.Manual;
            Left = gdiX / dx;
            Top = gdiY / dy;
            Width = gdiW / dx;
            Height = gdiH / dy;
        }

        public Rect? Result { get; private set; }

        private Rect _rectDip;
        private bool _dragging;
        private Point _dragStartMouse;
        private Rect _dragStartRect;
        private double _dpiX = 1.0, _dpiY = 1.0;

        private const double MinW = 64;
        private const double MinH = 64;

        public DrawAreaScreenWindow()
        {
            InitializeComponent();
            this.SizeChanged += (_, __) => ReflowAndClamp();
            this.LocationChanged += (_, __) => ReflowAndClamp();
            this.DpiChanged += (_, __) => ReflowAndClamp();
            RootCanvas.SizeChanged += (_, __) => ReflowAndClamp();
        }

        private void ReflowAndClamp()
        {
            UpdateLayout();
            var w = RootCanvas.RenderSize.Width;
            var h = RootCanvas.RenderSize.Height;

            _rectDip.X = Clamp(_rectDip.X, 0, Math.Max(0, w - _rectDip.Width));
            _rectDip.Y = Clamp(_rectDip.Y, 0, Math.Max(0, h - _rectDip.Height));
            _rectDip.Width = Clamp(_rectDip.Width, MinW, w);
            _rectDip.Height = Clamp(_rectDip.Height, MinH, h);
            Redraw();
        }

        private void Window_Loaded(object sender, RoutedEventArgs e)
        {
            int gdiX = GetSystemMetrics(76);
            int gdiY = GetSystemMetrics(77);
            int gdiW = GetSystemMetrics(78);
            int gdiH = GetSystemMetrics(79);

            var src = PresentationSource.FromVisual(this);
            double dpiX = 1.0, dpiY = 1.0;
            if (src?.CompositionTarget != null)
            {
                var m = src.CompositionTarget.TransformToDevice;
                dpiX = m.M11;
                dpiY = m.M22;
            }

            _dpiX = dpiX;
            _dpiY = dpiY;

            WindowState = WindowState.Normal;
            WindowStartupLocation = WindowStartupLocation.Manual;
            Left = gdiX / dpiX;
            Top = gdiY / dpiY;
            Width = gdiW / dpiX;
            Height = gdiH / dpiY;

            UpdateLayout();

            double cw = (RootCanvas?.ActualWidth > 0) ? RootCanvas.ActualWidth : ActualWidth;
            double ch = (RootCanvas?.ActualHeight > 0) ? RootCanvas.ActualHeight : ActualHeight;

            double w = Math.Min(1280, cw * 0.6);
            double h = Math.Min(720, ch * 0.6);
            _rectDip = new Rect(
                (cw - w) / 2,
                (ch - h) / 2,
                w, h
            );

            Redraw();
        }

        protected override void OnDpiChanged(DpiScale oldDpi, DpiScale newDpi)
        {
            base.OnDpiChanged(oldDpi, newDpi);

            int gdiX = GetSystemMetrics(76), gdiY = GetSystemMetrics(77),
                gdiW = GetSystemMetrics(78), gdiH = GetSystemMetrics(79);

            Left = gdiX / newDpi.DpiScaleX;
            Top = gdiY / newDpi.DpiScaleY;
            Width = gdiW / newDpi.DpiScaleX;
            Height = gdiH / newDpi.DpiScaleY;

            _dpiX = newDpi.DpiScaleX;
            _dpiY = newDpi.DpiScaleY;

            UpdateLayout();

            _rectDip.X = Clamp(_rectDip.X, 0, RootCanvas.ActualWidth - _rectDip.Width);
            _rectDip.Y = Clamp(_rectDip.Y, 0, RootCanvas.ActualHeight - _rectDip.Height);
            _rectDip.Width = Clamp(_rectDip.Width, MinW, RootCanvas.ActualWidth);
            _rectDip.Height = Clamp(_rectDip.Height, MinH, RootCanvas.ActualHeight);
            Redraw();
        }

        private static (int gdiX, int gdiY, int gdiW, int gdiH) GetVirtualGdi()
        {
            int gdiX = GetSystemMetrics(76);
            int gdiY = GetSystemMetrics(77);
            int gdiW = GetSystemMetrics(78);
            int gdiH = GetSystemMetrics(79);
            return (gdiX, gdiY, gdiW, gdiH);
        }

        private Rect GetDipBoundsForVirtualScreen()
        {
            var (gdiX, gdiY, gdiW, gdiH) = GetVirtualGdi();

            var tlPx = new Point(gdiX, gdiY);
            var brPx = new Point(gdiX + gdiW, gdiY + gdiH);

            var tlDip = this.PointFromScreen(tlPx);
            var brDip = this.PointFromScreen(brPx);

            return new Rect(tlDip, brDip);
        }

        private (double leftPx, double topPx, double rightPx, double bottomPx) CurrentRectToScreenPx()
        {
            var tlPx = this.PointToScreen(new Point(_rectDip.X, _rectDip.Y));
            var brPx = this.PointToScreen(new Point(_rectDip.Right, _rectDip.Bottom));
            return (tlPx.X, tlPx.Y, brPx.X, brPx.Y);
        }

        private void Redraw()
        {
            Canvas.SetLeft(SelectionRect, _rectDip.X);
            Canvas.SetTop(SelectionRect, _rectDip.Y);
            SelectionRect.Width = _rectDip.Width;
            SelectionRect.Height = _rectDip.Height;

            var (lpx, tpx, rpx, bpx) = CurrentRectToScreenPx();
            int pxW = Math.Max(0, (int)Math.Round(rpx - lpx));
            int pxH = Math.Max(0, (int)Math.Round(bpx - tpx));
            SizeText.Text = $"{pxW} x {pxH}";

            if (double.IsNaN(Hud.ActualWidth) || Hud.ActualWidth <= 0 ||
                double.IsNaN(Hud.ActualHeight) || Hud.ActualHeight <= 0)
            {
                Hud.Measure(new Size(double.PositiveInfinity, double.PositiveInfinity));
                Hud.Arrange(new Rect(new Size(Hud.DesiredSize.Width, Hud.DesiredSize.Height)));
            }

            double hudX = _rectDip.X + (_rectDip.Width - Hud.ActualWidth) / 2;
            double hudY = _rectDip.Y + (_rectDip.Height - Hud.ActualHeight) / 2;

            Canvas.SetLeft(Hud, hudX);
            Canvas.SetTop(Hud, hudY);

            PlaceHandle(HandleNW, _rectDip.X - HandleNW.Width / 2, _rectDip.Y - HandleNW.Height / 2);
            PlaceHandle(HandleN, _rectDip.X + _rectDip.Width / 2 - HandleN.Width / 2, _rectDip.Y - HandleN.Height / 2);
            PlaceHandle(HandleNE, _rectDip.Right - HandleNE.Width / 2, _rectDip.Y - HandleNE.Height / 2);
            PlaceHandle(HandleE, _rectDip.Right - HandleE.Width / 2, _rectDip.Y + _rectDip.Height / 2 - HandleE.Height / 2);
            PlaceHandle(HandleSE, _rectDip.Right - HandleSE.Width / 2, _rectDip.Bottom - HandleSE.Height / 2);
            PlaceHandle(HandleS, _rectDip.X + _rectDip.Width / 2 - HandleS.Width / 2, _rectDip.Bottom - HandleS.Height / 2);
            PlaceHandle(HandleSW, _rectDip.X - HandleSW.Width / 2, _rectDip.Bottom - HandleSW.Height / 2);
            PlaceHandle(HandleW, _rectDip.X - HandleW.Width / 2, _rectDip.Y + _rectDip.Height / 2 - HandleW.Height / 2);

            Panel.SetZIndex(SelectionRect, 10);
            Panel.SetZIndex(Hud, 20);
            Panel.SetZIndex(HandleNW, 30);
            Panel.SetZIndex(HandleN, 30);
            Panel.SetZIndex(HandleNE, 30);
            Panel.SetZIndex(HandleE, 30);
            Panel.SetZIndex(HandleSE, 30);
            Panel.SetZIndex(HandleS, 30);
            Panel.SetZIndex(HandleSW, 30);
            Panel.SetZIndex(HandleW, 30);
        }

        private void PlaceHandle(Thumb t, double x, double y)
        {
            Canvas.SetLeft(t, x);
            Canvas.SetTop(t, y);
        }

        private void SelectionRect_MouseLeftButtonDown(object sender, MouseButtonEventArgs e)
        {
            _dragging = true;
            _dragStartMouse = e.GetPosition(RootCanvas);
            _dragStartRect = _rectDip;
            SelectionRect.CaptureMouse();
            e.Handled = true;
        }

        private void SelectionRect_MouseMove(object sender, MouseEventArgs e)
        {
            if (!_dragging) return;

            var p = e.GetPosition(RootCanvas);
            var dx = p.X - _dragStartMouse.X;
            var dy = p.Y - _dragStartMouse.Y;

            var r = _dragStartRect;

            Rect dipBounds = GetDipBoundsForVirtualScreen();

            double maxX = Math.Max(0, dipBounds.Right - r.Width);
            double maxY = Math.Max(0, dipBounds.Bottom - r.Height);
            r.X = Clamp(r.X + dx, dipBounds.Left, maxX);
            r.Y = Clamp(r.Y + dy, dipBounds.Top, maxY);

            _rectDip = r;
            Redraw();
        }


        private void SelectionRect_MouseLeftButtonUp(object sender, MouseButtonEventArgs e)
        {
            if (_dragging)
            {
                _dragging = false;
                SelectionRect.ReleaseMouseCapture();
                e.Handled = true;
            }
        }

        private void ResizeByEdges(double left = 0, double top = 0, double right = 0, double bottom = 0)
        {
            var r = _rectDip;
            Rect dipBounds = GetDipBoundsForVirtualScreen();

            // esquerda
            if (left != 0)
            {
                double newX = r.X + left;
                double maxX = r.Right - MinW;
                newX = Clamp(newX, dipBounds.Left, maxX);
                r.Width = r.Right - newX;
                r.X = newX;
            }
            // topo
            if (top != 0)
            {
                double newY = r.Y + top;
                double maxY = r.Bottom - MinH;
                newY = Clamp(newY, dipBounds.Top, maxY);
                r.Height = r.Bottom - newY;
                r.Y = newY;
            }
            // direita
            if (right != 0)
            {
                double newRight = Clamp(r.Right + right, r.X + MinW, dipBounds.Right);
                r.Width = newRight - r.X;
            }
            // base
            if (bottom != 0)
            {
                double newBottom = Clamp(r.Bottom + bottom, r.Y + MinH, dipBounds.Bottom);
                r.Height = newBottom - r.Y;
            }

            _rectDip = r;
            Redraw();
        }


        private static double Clamp(double v, double min, double max) => (v < min) ? min : (v > max) ? max : v;

        private void Window_PreviewMouseLeftButtonUp(object sender, MouseButtonEventArgs e)
        {
            if (_dragging)
            {
                _dragging = false;
                try { SelectionRect.ReleaseMouseCapture(); } catch { }
                e.Handled = false;
            }
        }

        // ====== teclas ======
        private void Window_KeyDown(object sender, KeyEventArgs e)
        {
            if (e.Key == Key.Escape)
            {
                Result = null;
                DialogResult = false;
                Close();
            }
            else if (e.Key == Key.Enter)
            {
                var (leftPx, topPx, rightPx, bottomPx) = CurrentRectToScreenPx();

                var (gdiX, gdiY, gdiW, gdiH) = GetVirtualGdi();
                double vLeft = gdiX, vTop = gdiY, vRight = gdiX + gdiW, vBottom = gdiY + gdiH;

                leftPx = Math.Max(leftPx, vLeft);
                topPx = Math.Max(topPx, vTop);
                rightPx = Math.Min(rightPx, vRight);
                bottomPx = Math.Min(bottomPx, vBottom);

                int xi = (int)Math.Floor(leftPx);
                int yi = (int)Math.Floor(topPx);
                int ri = (int)Math.Ceiling(rightPx);
                int bi = (int)Math.Ceiling(bottomPx);

                int wi = Math.Max(0, ri - xi);
                int hi = Math.Max(0, bi - yi);

                if (wi < 2) wi = 2;
                if (hi < 2) hi = 2;
                if ((wi & 1) == 1) wi--;
                if ((hi & 1) == 1) hi--;

                if (xi + wi > vRight) xi = (int)(vRight - wi);
                if (yi + hi > vBottom) yi = (int)(vBottom - hi);
                if (xi < vLeft) xi = (int)vLeft;
                if (yi < vTop) yi = (int)vTop;

                Result = new Rect(xi, yi, wi, hi);
                DialogResult = true;
                Close();
            }
            else if (e.Key == Key.Left)
            {
                var b = GetDipBoundsForVirtualScreen();
                _rectDip.X = Clamp(_rectDip.X - 1, b.Left, Math.Max(b.Left, b.Right - _rectDip.Width));
                Redraw();
            }
            else if (e.Key == Key.Right)
            {
                var b = GetDipBoundsForVirtualScreen();
                _rectDip.X = Clamp(_rectDip.X + 1, b.Left, Math.Max(b.Left, b.Right - _rectDip.Width));
                Redraw();
            }
            else if (e.Key == Key.Up)
            {
                var b = GetDipBoundsForVirtualScreen();
                _rectDip.Y = Clamp(_rectDip.Y - 1, b.Top, Math.Max(b.Top, b.Bottom - _rectDip.Height));
                Redraw();
            }
            else if (e.Key == Key.Down)
            {
                var b = GetDipBoundsForVirtualScreen();
                _rectDip.Y = Clamp(_rectDip.Y + 1, b.Top, Math.Max(b.Top, b.Bottom - _rectDip.Height));
                Redraw();
            }
        }
    }
}

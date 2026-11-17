using System;
using System.Diagnostics;
using System.Windows;
using System.Windows.Controls;
using System.Windows.Interop;
using System.Windows.Media;
using System.Windows.Media.Imaging;
using NessStudio.Interop.Native;
using NessStudio.Recording;
using NessStudio.ViewModel.Helpers;

namespace NessStudio.Presentation;

/// <summary>Presents an isolated native pool. No source, capture loop, recording clock or media frame belongs to a view model.</summary>
public sealed class NativePreviewControl : Image
{
    /// <summary>Identifies the window-owned source workspace.</summary>
    public static readonly DependencyProperty WorkspaceProperty = DependencyProperty.Register(nameof(Workspace),typeof(NativeRecordingWorkspace),typeof(NativePreviewControl),new PropertyMetadata(null,WorkspaceChanged));
    /// <summary>Identifies the requested source, one for screen or two for camera.</summary>
    public static readonly DependencyProperty SourceIdProperty = DependencyProperty.Register(nameof(SourceId),typeof(ulong),typeof(NativePreviewControl),new PropertyMetadata(1UL,(sender,_) => ((NativePreviewControl)sender).RequestRebind()));
    private static readonly DependencyPropertyKey PresentationErrorPropertyKey = DependencyProperty.RegisterReadOnly(nameof(PresentationError),typeof(string),typeof(NativePreviewControl),new PropertyMetadata(null));
    /// <summary>Identifies the isolated presentation error used by the View's status overlay.</summary>
    public static readonly DependencyProperty PresentationErrorProperty = PresentationErrorPropertyKey.DependencyProperty;
    /// <summary>Gets or sets the window-owned presentation workspace.</summary>
    public NativeRecordingWorkspace? Workspace { get => (NativeRecordingWorkspace?)GetValue(WorkspaceProperty); set => SetValue(WorkspaceProperty,value); }
    /// <summary>Gets or sets the native source identity.</summary>
    public ulong SourceId { get => (ulong)GetValue(SourceIdProperty); set => SetValue(SourceIdProperty,value); }
    private NativePreview? _preview;
    private NativePreviewFrame? _displayed;
    private D3DImage? _image;
    private WriteableBitmap? _bitmap;
    private long _lastRender;
    private bool _subscribed, _rebind = true;
    /// <summary>Gets how many actual frames were presented by this control.</summary>
    public long PresentedFrames { get; private set; }
    /// <summary>Gets the presentation mode observed by this control.</summary>
    public string PresentationKind { get; private set; } = "none";
    /// <summary>Gets the isolated presentation diagnostic, if any.</summary>
    public string? PresentationError { get => (string?)GetValue(PresentationErrorProperty); private set => SetValue(PresentationErrorPropertyKey,value); }
    /// <summary>Gets the last pool generation actually presented.</summary>
    public ulong PresentedGeneration { get; private set; }
    /// <summary>Gets the actual bounded presentation width last accepted.</summary>
    public int PresentedWidth { get; private set; }
    /// <summary>Gets the actual bounded presentation height last accepted.</summary>
    public int PresentedHeight { get; private set; }
    /// <summary>Gets bytes explicitly copied into the reused WPF CPU bitmap.</summary>
    public long CpuWriteBytes { get; private set; }
    /// <summary>Gets the number of CPU bitmap allocations, independently of presented frames.</summary>
    public long CpuBitmapAllocations { get; private set; }
    /// <summary>Gets completed D3D9 surface bindings; WPF/driver composition cost is measured separately.</summary>
    public long GpuSurfaceBindings { get; private set; }
    /// <summary>Initializes a presenter whose lifetime follows visibility, independently of native recording.</summary>
    public NativePreviewControl()
    {
        Loaded += (_,_) => UpdateRendering();
        Unloaded += (_,_) => { Unsubscribe(); Detach(); };
        IsVisibleChanged += (_,_) => UpdateRendering();
        Stretch = Stretch.Uniform;
    }
    private static void WorkspaceChanged(DependencyObject sender,DependencyPropertyChangedEventArgs change)
    {
        var control = (NativePreviewControl)sender;
        if (change.OldValue is NativeRecordingWorkspace old) old.PresentationChanged -= control.OnPresentationChanged;
        if (change.NewValue is NativeRecordingWorkspace current) current.PresentationChanged += control.OnPresentationChanged;
        control.RequestRebind();
    }
    private void OnPresentationChanged(object? sender,EventArgs args)
    {
        if (!Dispatcher.CheckAccess()) { if (!Dispatcher.HasShutdownStarted) Dispatcher.BeginInvoke(RequestRebind); return; }
        RequestRebind();
    }
    private void RequestRebind() { Detach(); _rebind = true; UpdateRendering(); }
    private void UpdateRendering()
    {
        try
        {
            bool visible = IsLoaded && IsVisible;
            _preview?.SetEnabled(visible);
            if (visible)
            { if (!_subscribed) { CompositionTarget.Rendering += RenderFrame; _subscribed = true; } }
            else { Unsubscribe(); ClearPresentation(); }
        }
        catch (Exception error)
        { PresentationError = error.Message; DebugLog.Write("[NativePreview] " + error); Detach(); }
    }
    private void Unsubscribe() { if (_subscribed) { CompositionTarget.Rendering -= RenderFrame; _subscribed = false; } }
    /// <inheritdoc />
    protected override void OnRenderSizeChanged(SizeChangedInfo info) { base.OnRenderSizeChanged(info); }
    /// <inheritdoc />
    protected override void OnDpiChanged(DpiScale oldDpi,DpiScale newDpi) { base.OnDpiChanged(oldDpi,newDpi); }
    private void FrontBufferChanged(object? sender,DependencyPropertyChangedEventArgs args) => _lastRender = 0;
    private void RenderFrame(object? sender,EventArgs args)
    {
        long now = Stopwatch.GetTimestamp();
        if (now-_lastRender < Stopwatch.Frequency/15) return;
        _lastRender = now;
        try
        {
            if (_rebind) { Detach(); _preview = Workspace?.Attach(SourceId); _rebind = false; PresentationError = _preview is null ? Workspace?.Error : null; }
            var next = _preview?.TryAcquire();
            if (next is null) return;
            try
            {
            if (next.SourceId != SourceId) throw new InvalidOperationException("Presentation source identity changed.");
            if (!next.IsGpu)
            {
                if (_bitmap is null || _bitmap.PixelWidth != next.Width || _bitmap.PixelHeight != next.Height)
                {
                    _bitmap = new WriteableBitmap(next.Width,next.Height,96,96,PixelFormats.Bgra32,null);
                    CpuBitmapAllocations++;
                }
                _bitmap.WritePixels(new Int32Rect(0,0,next.Width,next.Height),next.Pointer,next.Bytes,next.Stride);
                CpuWriteBytes += next.Bytes;
                Source = _bitmap; PresentationKind = "CPU BGRA WriteableBitmap";
            }
            else if (!PresentGpu(next)) return;
            PresentedGeneration = next.Generation; PresentedWidth = next.Width; PresentedHeight = next.Height; PresentedFrames++;
            }
            finally { if (!ReferenceEquals(next,_displayed)) next.Dispose(); }
        }
        catch (NativeEngineException error) when (error.ResultCode == -5)
        { PresentationError = error.Message; Detach(); _rebind = true; _lastRender = now + Stopwatch.Frequency / 4; }
        catch (Exception error)
        { PresentationError = error.Message; DebugLog.Write("[NativePreview] " + error); Detach(); }
    }
    private bool PresentGpu(NativePreviewFrame frame)
    {
        _image ??= NewD3DImage();
        if (!_image.IsFrontBufferAvailable || !_image.TryLock(new Duration(TimeSpan.Zero))) return false;
        try
        {
            // Release the previous lease only after WPF has unbound that surface under its lock.
            _image.SetBackBuffer(D3DResourceType.IDirect3DSurface9,IntPtr.Zero);
            _displayed?.Dispose(); _displayed = null;
            _image.SetBackBuffer(D3DResourceType.IDirect3DSurface9,frame.Pointer);
            // Acquire returns a disposable lease. Retain it here instead of returning its pool slot after RenderFrame.
            _displayed = frame;
            _image.AddDirtyRect(new Int32Rect(0,0,frame.Width,frame.Height)); Source = _image;
            PresentationKind = "D3D11 to D3D9Ex D3DImage";
            GpuSurfaceBindings++;
            return true;
        }
        finally { _image.Unlock(); }
    }
    private D3DImage NewD3DImage() { var image = new D3DImage(); image.IsFrontBufferAvailableChanged += FrontBufferChanged; return image; }
    private void ClearPresentation()
    {
        if (_image is not null)
        {
            _image.Lock();
            try { _image.SetBackBuffer(D3DResourceType.IDirect3DSurface9,IntPtr.Zero); }
            finally { _image.Unlock(); }
            _image.IsFrontBufferAvailableChanged -= FrontBufferChanged; _image = null;
        }
        _displayed?.Dispose(); _displayed = null; Source = null;
    }
    private void Detach()
    {
        ClearPresentation(); _preview?.Dispose(); _preview = null; _rebind = true;
    }
}

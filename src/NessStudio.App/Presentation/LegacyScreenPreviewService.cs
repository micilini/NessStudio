using System.Diagnostics;
using System.Windows;
using System.Windows.Interop;
using System.Windows.Media.Imaging;
using System.Windows.Threading;
using NessStudio.Interop.Legacy;
using NessStudio.Models;
using NessStudio.Recording;

namespace NessStudio.Presentation;

/// <summary>Owns the legacy GDI preview worker. This implementation is excluded from the native application build.</summary>
public sealed class LegacyScreenPreviewService : IScreenPreviewService
{
    private readonly Dispatcher _dispatcher;
    private readonly object _sync = new();
    private readonly AutoResetEvent _wake = new(false);
    private readonly TaskCompletionSource _finished = new(TaskCreationOptions.RunContinuationsAsynchronously);
    private readonly Thread _worker;
    private MonitorDescriptor? _requested;
    private bool _oneShot, _closing;
    private long _generation, _captured, _presented, _dropped, _captureTicks;
    private long? _cpuHns;
    private int _pending, _workerThreadId, _presentationThreadId, _wakeDisposed;
    private PreviewPresentation? _latest;

    /// <summary>Initializes one bounded worker and presentation mailbox for this window.</summary>
    public LegacyScreenPreviewService(Dispatcher dispatcher)
    {
        _dispatcher = dispatcher ?? throw new ArgumentNullException(nameof(dispatcher));
        _worker = new Thread(CaptureLoop) { IsBackground = true, Name = "NessStudio legacy screen preview" };
        _worker.SetApartmentState(ApartmentState.MTA);
        _worker.Start();
    }
    public event EventHandler<PreviewPresentation>? FrameReady;
    public event EventHandler<Exception>? Failed;
    public ScreenPreviewStatistics Statistics
    {
        get
        {
            lock (_sync) return new(_captured, _presented, _dropped,
                TimeSpan.FromSeconds((double)_captureTicks / Stopwatch.Frequency),
                _cpuHns is { } cpu ? TimeSpan.FromTicks(cpu) : null, _workerThreadId, _presentationThreadId);
        }
    }
    public void Start(MonitorDescriptor monitor) => Request(monitor, false);
    public void CaptureOnce(MonitorDescriptor monitor)
    {
        lock (_sync) Request(monitor, _requested is null);
    }
    private void Request(MonitorDescriptor monitor, bool oneShot)
    {
        ArgumentNullException.ThrowIfNull(monitor);
        lock (_sync)
        {
            ObjectDisposedException.ThrowIf(_closing, this);
            _requested = monitor; _oneShot = oneShot; _generation++; _latest = null;
            _wake.Set();
        }
    }
    public void Stop()
    {
        lock (_sync)
        {
            if (_closing) return;
            _requested = null; _latest = null; _generation++;
            _wake.Set();
        }
    }
    private void CaptureLoop()
    {
        System.Drawing.Bitmap? bitmap = null;
        System.Drawing.Graphics? graphics = null;
        long? cpuStart = ThreadCpuTime.ReadCurrentThreadHns();
        _workerThreadId = Environment.CurrentManagedThreadId;
        try
        {
            while (true)
            {
                MonitorDescriptor? monitor;
                long generation;
                bool once;
                lock (_sync) { if (_closing) break; monitor = _requested; generation = _generation; once = _oneShot; }
                if (monitor is null)
                {
                    graphics?.Dispose(); graphics = null;
                    bitmap?.Dispose(); bitmap = null;
                    _wake.WaitOne();
                    continue;
                }
                long started = Stopwatch.GetTimestamp();
                try
                {
                    var bounds = LegacyGeometry.ToGdi(monitor.Bounds);
                    if (bitmap is null || bitmap.Width != bounds.Width || bitmap.Height != bounds.Height)
                    {
                        graphics?.Dispose(); bitmap?.Dispose();
                        bitmap = new System.Drawing.Bitmap(bounds.Width, bounds.Height, System.Drawing.Imaging.PixelFormat.Format24bppRgb);
                        graphics = System.Drawing.Graphics.FromImage(bitmap);
                        graphics.CompositingMode = System.Drawing.Drawing2D.CompositingMode.SourceCopy;
                    }
                    graphics!.CopyFromScreen(bounds.X, bounds.Y, 0, 0, bounds.Size, System.Drawing.CopyPixelOperation.SourceCopy);
                    BitmapSource image;
                    using (var handle = new GdiBitmapHandle(bitmap.GetHbitmap()))
                    {
                        image = Imaging.CreateBitmapSourceFromHBitmap(handle.DangerousGetHandle(), nint.Zero,
                            Int32Rect.Empty, BitmapSizeOptions.FromEmptyOptions());
                        image.Freeze();
                    }
                    lock (_sync)
                    {
                        _captured++;
                        _captureTicks += Stopwatch.GetTimestamp() - started;
                        long? cpuNow = ThreadCpuTime.ReadCurrentThreadHns();
                        _cpuHns = cpuStart is { } first && cpuNow is { } last ? last - first : null;
                        if (generation == _generation && !_closing)
                        {
                            if (_latest is not null) _dropped++;
                            _latest = new(image, PreviewPresentationKind.CpuBitmap, generation);
                            if (once) _requested = null;
                        }
                    }
                    QueuePresentation();
                }
                catch (Exception ex)
                {
                    lock (_sync) { if (generation == _generation) _requested = null; }
                    try
                    {
                        if (!_dispatcher.HasShutdownStarted)
                            _dispatcher.BeginInvoke(() =>
                            {
                                lock (_sync) { if (_closing || generation != _generation) return; }
                                Failed?.Invoke(this, ex);
                            }, DispatcherPriority.Background);
                    }
                    catch (InvalidOperationException) { }
                }
                int delay = Math.Max(1, 100 - (int)Stopwatch.GetElapsedTime(started).TotalMilliseconds);
                _wake.WaitOne(delay);
            }
        }
        finally
        {
            graphics?.Dispose(); bitmap?.Dispose();
            _finished.TrySetResult();
        }
    }
    private void QueuePresentation()
    {
        lock (_sync)
        {
            if (_closing || _latest is null || _pending != 0 || _dispatcher.HasShutdownStarted) return;
            _pending = 1;
        }
        try
        {
            _dispatcher.BeginInvoke(() =>
            {
                PreviewPresentation? presentation;
                lock (_sync)
                {
                    presentation = _latest; _latest = null; _pending = 0;
                    if (_closing || presentation?.Generation != _generation) return;
                    _presented++; _presentationThreadId = Environment.CurrentManagedThreadId;
                }
                FrameReady?.Invoke(this, presentation);
            }, DispatcherPriority.Background);
        }
        catch (InvalidOperationException) { lock (_sync) { _pending = 0; _latest = null; } }
    }
    public async ValueTask DisposeAsync()
    {
        lock (_sync)
        {
            if (!_closing) { _closing = true; _generation++; _requested = null; _latest = null; _wake.Set(); }
        }
        await _finished.Task.ConfigureAwait(false);
        if (Interlocked.Exchange(ref _wakeDisposed, 1) == 0) _wake.Dispose();
        // The worker has released all GDI objects; no Dispatcher callback owns a native handle.
        FrameReady = null; Failed = null;
    }
}

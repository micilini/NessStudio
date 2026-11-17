namespace NessStudio.Interop.Native;

/// <summary>A bounded native presentation consumer. It never owns or controls a recording clock.</summary>
public sealed class NativePreview : IDisposable
{
    private readonly PreviewHandle _handle;
    private readonly bool _gpu;
    private int _disposed;
    private NativePreview(PreviewHandle handle, bool gpu) { _handle = handle; _gpu = gpu; }
    internal static unsafe NativePreview Attach(SessionHandle session, ulong source, bool gpu)
    {
        nss_preview_config_v1 config = new() { size = (uint)sizeof(nss_preview_config_v1), version = 1,
            source_id = source, max_width = 640, max_height = 360, fps_num = 15, fps_den = 1, slots = 3, channel = gpu ? 2u : 1u };
        var error = NativeEngine.NewError();
        int result = NativeMethods.PreviewAttach(session, ref config, out var handle, ref error);
        if (result != 0) { handle.Dispose(); NativeEngine.Throw(result, error); }
        return new(handle, gpu);
    }
    /// <summary>Suspends production while retaining the bounded presentation pool for the next visible frame.</summary>
    public void SetEnabled(bool enabled)
    {
        ObjectDisposedException.ThrowIf(Volatile.Read(ref _disposed) != 0, this);
        var error = NativeEngine.NewError();
        int result = NativeMethods.PreviewSetEnabled(_handle, enabled ? 1u : 0u, ref error);
        if (result != 0) NativeEngine.Throw(result, error);
    }
    /// <summary>Acquires the newest completed immutable frame without waiting. The caller must release it after presentation use.</summary>
    public unsafe NativePreviewFrame? TryAcquire()
    {
        ObjectDisposedException.ThrowIf(Volatile.Read(ref _disposed) != 0, this);
        var error = NativeEngine.NewError();
        if (_gpu)
        {
            nss_gpu_surface_v1 frame = new() { size = (uint)sizeof(nss_gpu_surface_v1), version = 1 };
            int result = NativeMethods.GpuPreviewAcquire(_handle, out var lease, ref frame, ref error);
            if (result == 3) { lease.Dispose(); return null; }
            if (result != 0) { lease.Dispose(); NativeEngine.Throw(result, error); }
            if (frame.surface_d3d9 == 0 || frame.width is 0 or > 640 || frame.height is 0 or > 360)
            { lease.Dispose(); throw new InvalidDataException("Invalid GPU presentation envelope."); }
            return new(lease, frame);
        }
        else
        {
            nss_preview_frame_v1 frame = new() { size = (uint)sizeof(nss_preview_frame_v1), version = 1 };
            int result = NativeMethods.PreviewAcquire(_handle, out var lease, ref frame, ref error);
            if (result == 3) { lease.Dispose(); return null; }
            if (result != 0) { lease.Dispose(); NativeEngine.Throw(result, error); }
            if (frame.data == 0 || frame.pixel_format != 1 || frame.width is 0 or > 640 || frame.height is 0 or > 360 ||
                frame.stride < frame.width * 4 || frame.bytes != (ulong)frame.stride * frame.height || frame.bytes > 640UL * 360 * 4)
            { lease.Dispose(); throw new InvalidDataException("Invalid CPU BGRA presentation envelope."); }
            return new(lease, frame);
        }
    }
    /// <summary>Detaches this consumer; frame leases already acquired remain valid until released.</summary>
    public void Dispose()
    {
        if (Interlocked.Exchange(ref _disposed, 1) == 0) _handle.Dispose();
    }
}

/// <summary>Retains presentation storage. Its pointer is valid only until Dispose; no recording buffer is exposed.</summary>
public sealed class NativePreviewFrame : IDisposable
{
    private readonly PreviewLeaseHandle _lease;
    private readonly nint _pointer;
    /// <summary>Gets the originating session identity.</summary>
    public ulong SessionId { get; }
    /// <summary>Gets the source identity, one for screen or two for camera.</summary>
    public ulong SourceId { get; }
    /// <summary>Gets the presentation pool generation.</summary>
    public ulong Generation { get; }
    /// <summary>Gets the immutable source frame identity.</summary>
    public ulong FrameId { get; }
    /// <summary>Gets the common QPC acquisition timestamp in 100 ns units.</summary>
    public long CaptureTimeHns { get; }
    /// <summary>Gets the pixel width.</summary>
    public int Width { get; }
    /// <summary>Gets the pixel height.</summary>
    public int Height { get; }
    /// <summary>Gets the CPU row stride, zero for GPU surfaces.</summary>
    public int Stride { get; }
    /// <summary>Gets the CPU buffer length, zero for GPU surfaces.</summary>
    public int Bytes { get; }
    /// <summary>Gets whether Pointer refers to IDirect3DSurface9 rather than BGRA bytes.</summary>
    public bool IsGpu { get; }
    /// <summary>Gets a borrowed presentation pointer while this lease is alive.</summary>
    public nint Pointer { get { ObjectDisposedException.ThrowIf(_lease.IsClosed, this); return _pointer; } }
    internal NativePreviewFrame(PreviewLeaseHandle lease, nss_preview_frame_v1 frame)
    {
        _lease = lease; _pointer = frame.data; SessionId = frame.session_id; SourceId = frame.source_id;
        Generation = frame.generation; FrameId = frame.frame_id; CaptureTimeHns = frame.capture_time_hns;
        Width = checked((int)frame.width); Height = checked((int)frame.height); Stride = checked((int)frame.stride); Bytes = checked((int)frame.bytes);
    }
    internal NativePreviewFrame(PreviewLeaseHandle lease, nss_gpu_surface_v1 frame)
    {
        _lease = lease; _pointer = frame.surface_d3d9; IsGpu = true; SessionId = frame.session_id; SourceId = frame.source_id;
        Generation = frame.generation; FrameId = frame.frame_id; CaptureTimeHns = frame.capture_time_hns;
        Width = checked((int)frame.width); Height = checked((int)frame.height);
    }
    /// <summary>Releases the presentation slot after all pointer use has finished.</summary>
    public void Dispose() => _lease.Dispose();
}

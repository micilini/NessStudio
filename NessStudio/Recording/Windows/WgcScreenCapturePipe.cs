using NessStudio.Models;
using System;
using System.Buffers;
using System.Collections.Concurrent;
using System.IO;
using System.Runtime.InteropServices;
using System.Threading;
using Windows.Graphics;
using Windows.Graphics.Capture;
using Windows.Graphics.DirectX;
using Windows.Graphics.DirectX.Direct3D11;
using WinRT;
using NessStudio.ViewModel.Helpers;

namespace NessStudio.Recording.Windows
{
    public sealed class WgcScreenCapturePipe : IDisposable
    {
        private readonly ScreenRegion _region;
        private readonly RecordingOutputPaths _paths;
        private int _segmentIndex;
        private readonly int _fps;
        private readonly bool _drawMouse;
        private readonly int _warmupMilliseconds;
        private bool _sessionInitialized;

        private GraphicsCaptureItem _item;
        private Direct3D11CaptureFramePool _framePool;
        private GraphicsCaptureSession _session;
        private IDirect3DDevice _wgcDevice;
        private IntPtr _d3d11DevicePtr;
        private IntPtr _d3d11ContextPtr;
        private IntPtr _stagingTexPtr;
        private int _stagingW, _stagingH;
        private NessMuxerWriter _mf;

        private long _segmentStartTimestampHns;
        private readonly System.Collections.Generic.List<(long PauseHns, long ResumeHns)> _pauseIntervals
            = new System.Collections.Generic.List<(long, long)>();
        private long _lastPauseTimestampHns;

        private byte[] _frameBuffer;
        private int _bytesPerFrame;
        private int _nv12BytesPerFrame;
        private int _cropX, _cropY, _cropW, _cropH;
        private volatile bool _stopping;
        private readonly object _frameLock = new object();
        private int _framesWritten;
        private int _frameErrors;
        private bool _loggedFirstFrame;
        private bool _disposed;
        private DateTime _captureStartedAtUtc;
        private DateTime? _firstStableFrameAtUtc;
        private int _warmupDiscardedFrames;
        private DateTime _segmentFirstFrameTime;
        private long _segmentBasePts;

        private readonly struct QueuedFrame
        {
            public readonly byte[] Buffer;
            public readonly bool IsFirst;
            public readonly int SegIdx;
            public readonly long PauseHns;
            public readonly DateTime CaptureStart;
            public readonly int WarmupDiscarded;
            public readonly int RowPitch;
            public readonly long PtsHns;

            public QueuedFrame(byte[] buf, bool isFirst, int segIdx, long pauseHns,
                               DateTime captureStart, int warmupDiscarded, int rowPitch, long ptsHns)
            {
                Buffer = buf;
                IsFirst = isFirst;
                SegIdx = segIdx;
                PauseHns = pauseHns;
                CaptureStart = captureStart;
                WarmupDiscarded = warmupDiscarded;
                RowPitch = rowPitch;
                PtsHns = ptsHns;
            }
        }

        private readonly ConcurrentQueue<QueuedFrame> _encodeQueue = new ConcurrentQueue<QueuedFrame>();
        private readonly ConcurrentBag<byte[]> _bufferPool = new ConcurrentBag<byte[]>();
        private readonly SemaphoreSlim _encodeSignal = new SemaphoreSlim(0, int.MaxValue);
        private Thread _encodeThread;
        private volatile bool _encodeThreadRunning;

        private const int PoolSize = 8;
        private const int MaxQueueSize = 3;

        private static readonly Guid IID_IGraphicsCaptureItem = new("79C3F95B-31F7-4EC2-A464-632EF5D30760");
        private static readonly Guid IID_ID3D11Texture2D = new("6f15aaf2-d208-4e89-9ab4-489535d34f9c");

        public WgcScreenCapturePipe(
            ScreenRegion region,
            RecordingOutputPaths paths,
            int fps = 30,
            bool drawMouse = true,
            int warmupMilliseconds = 1200)
        {
            _region = region ?? throw new ArgumentNullException(nameof(region));
            _paths = paths ?? throw new ArgumentNullException(nameof(paths));
            _fps = fps;
            _drawMouse = drawMouse;
            _warmupMilliseconds = Math.Max(0, warmupMilliseconds);
        }

        public void InitializeSession()
        {
            if (_sessionInitialized) return;
            DebugLog.Write("[WGC] InitializeSession begin");
            RecordingPerfProbe.Mark("wgc-session-init-begin");

            var abs = GetAbsoluteCaptureRect(_region);
            var center = new System.Drawing.Point(abs.X + abs.Width / 2, abs.Y + abs.Height / 2);
            IntPtr hmon = MonitorFromPoint(center, 2);
            DebugLog.Write($"[WGC] MonitorFromPoint hmon=0x{hmon.ToInt64():X}");
            if (hmon == IntPtr.Zero)
                throw new InvalidOperationException("MonitorFromPoint retornou zero.");
            if (!TryGetMonitorRect(hmon, out var mon))
                throw new InvalidOperationException("GetMonitorInfo falhou.");
            DebugLog.Write($"[WGC] Monitor L:{mon.Left} T:{mon.Top} W:{mon.Width} H:{mon.Height}");

            var fullAbs = GetAbsoluteCaptureRect(_region);
            _cropX = fullAbs.X - mon.Left;
            _cropY = fullAbs.Y - mon.Top;
            _cropW = fullAbs.Width;
            _cropH = fullAbs.Height;
            MakeEven(ref _cropW, ref _cropH);
            if (_cropX < 0) _cropX = 0;
            if (_cropY < 0) _cropY = 0;
            if (_cropX + _cropW > mon.Width) _cropW = Math.Max(16, mon.Width - _cropX);
            if (_cropY + _cropH > mon.Height) _cropH = Math.Max(16, mon.Height - _cropY);
            MakeEven(ref _cropW, ref _cropH);
            DebugLog.Write($"[WGC] Crop X:{_cropX} Y:{_cropY} W:{_cropW} H:{_cropH}");

            RecordingPerfProbe.Mark("wgc-d3d11-create-begin", "session-init");
            DebugLog.Write("[WGC] D3D11CreateDevice begin");
            int hr = D3D11CreateDevice(
                IntPtr.Zero, D3D_DRIVER_TYPE_HARDWARE, IntPtr.Zero,
                D3D11_CREATE_DEVICE_BGRA_SUPPORT,
                null, 0, D3D11_SDK_VERSION,
                out _d3d11DevicePtr, out _, out _d3d11ContextPtr);
            DebugLog.Write($"[WGC] D3D11CreateDevice hr=0x{hr:X8} dev=0x{_d3d11DevicePtr.ToInt64():X} ctx=0x{_d3d11ContextPtr.ToInt64():X}");
            if (hr < 0 || _d3d11DevicePtr == IntPtr.Zero)
                throw new COMException("D3D11CreateDevice falhou", hr);
            RecordingPerfProbe.Mark("wgc-d3d11-create-end", $"session-init hr=0x{hr:X8}");

            Guid dxgiGuid = typeof(IDXGIDevice).GUID;
            Marshal.QueryInterface(_d3d11DevicePtr, ref dxgiGuid, out IntPtr dxgiDevicePtr);
            if (dxgiDevicePtr == IntPtr.Zero)
                throw new InvalidOperationException("QueryInterface IDXGIDevice falhou.");
            try
            {
                int hrWd = CreateDirect3D11DeviceFromDXGIDevice(dxgiDevicePtr, out IntPtr winrtDevPtr);
                if (hrWd < 0 || winrtDevPtr == IntPtr.Zero)
                    throw new COMException("CreateDirect3D11DeviceFromDXGIDevice falhou", hrWd);
                try
                {
                    _wgcDevice = WinRT.MarshalInterface<IDirect3DDevice>.FromAbi(winrtDevPtr);
                    DebugLog.Write("[WGC] IDirect3DDevice WinRT OK");
                }
                finally { Marshal.Release(winrtDevPtr); }
            }
            finally { Marshal.Release(dxgiDevicePtr); }

            _bytesPerFrame = _cropW * _cropH * 4;
            _nv12BytesPerFrame = (_cropW * _cropH * 3) / 2;
            _frameBuffer = ArrayPool<byte>.Shared.Rent(_bytesPerFrame);

            for (int i = 0; i < PoolSize; i++)
                _bufferPool.Add(new byte[_nv12BytesPerFrame]);

            string continuousFile = _paths.ScreenContinuous();
            DebugLog.Write($"[WGC] creating persistent NessMuxerWriter => {continuousFile} | {_cropW}x{_cropH} @{_fps}fps");
            RecordingPerfProbe.Mark("wgc-writer-create-begin", $"file={System.IO.Path.GetFileName(continuousFile)}");
            _mf = new NessMuxerWriter();
            _mf.Start(continuousFile, _cropW, _cropH, _fps);
            RecordingPerfProbe.Mark("wgc-writer-create-end", $"file={System.IO.Path.GetFileName(continuousFile)}");
            DebugLog.Write("[WGC] persistent NessMuxerWriter created OK");

            _encodeThreadRunning = true;
            _encodeThread = new Thread(EncodeThreadProc)
            {
                Name = "NessStudio-Encoder",
                IsBackground = true,
                Priority = ThreadPriority.AboveNormal
            };
            _encodeThread.Start();
            DebugLog.Write("[WGC] encode thread started");

            _sessionInitialized = true;
            RecordingPerfProbe.Mark("wgc-session-init-end", $"crop={_cropW}x{_cropH}");
            DebugLog.Write("[WGC] InitializeSession end");
        }

        public void StartSegment(int segmentIndex)
        {
            if (!_sessionInitialized)
                throw new InvalidOperationException("InitializeSession() deve ser chamado antes de StartSegment().");

            _segmentIndex = segmentIndex;
            DebugLog.Write($"[WGC] ═══ StartSegment({segmentIndex}) begin ═══");
            DebugLog.Write($"[WGC] Thread='{Thread.CurrentThread.Name}' ApartmentState={Thread.CurrentThread.GetApartmentState()}");
            _captureStartedAtUtc = DateTime.UtcNow;
            _firstStableFrameAtUtc = null;
            _warmupDiscardedFrames = 0;
            _framesWritten = 0;
            _frameErrors = 0;
            _loggedFirstFrame = false;
            DebugLog.Write($"[WGC] Warmup configured => {_warmupMilliseconds}ms");

            RecordingPerfProbe.Mark("wgc-start-begin", $"segment={segmentIndex}");

            try
            {
                var abs = GetAbsoluteCaptureRect(_region);
                var center = new System.Drawing.Point(abs.X + abs.Width / 2, abs.Y + abs.Height / 2);
                IntPtr hmon = MonitorFromPoint(center, 2);
                if (hmon == IntPtr.Zero)
                    throw new InvalidOperationException("MonitorFromPoint retornou zero.");

                RecordingPerfProbe.Mark("wgc-capture-item-begin", $"segment={segmentIndex}");
                _item = CreateItemForMonitor(hmon);
                DebugLog.Write("[WGC] GraphicsCaptureItem OK");
                RecordingPerfProbe.Mark("wgc-capture-item-end", $"segment={segmentIndex}");

                if (_segmentIndex > 1 && _lastPauseTimestampHns > 0)
                {
                    DebugLog.Write($"[WGC] P3.2 PauseInterval deferred | pause={_lastPauseTimestampHns} | aguardando primeiro frame estável");
                    RecordingPerfProbe.Mark("wgc-p32-pause-deferred", $"segment={segmentIndex} pause={_lastPauseTimestampHns}");
                }
                DebugLog.Write($"[WGC] F3/F4 writer reused | currentTs={_mf?.CurrentTimestamp ?? 0L}");
                RecordingPerfProbe.Mark("wgc-mf-start-begin", $"segment={segmentIndex}");
                RecordingPerfProbe.Mark("wgc-mf-start-end", $"segment={segmentIndex}");

                _stopping = false;

                if (!TryGetMonitorRect(hmon, out var mon))
                    throw new InvalidOperationException("GetMonitorInfo falhou.");
                var monSize = new SizeInt32 { Width = mon.Width, Height = mon.Height };

                RecordingPerfProbe.Mark("wgc-framepool-begin", $"segment={segmentIndex} size={monSize.Width}x{monSize.Height}");
                DebugLog.Write($"[WGC] CreateFreeThreaded FramePool size={monSize.Width}x{monSize.Height}");
                _framePool = Direct3D11CaptureFramePool.CreateFreeThreaded(
                    _wgcDevice,
                    DirectXPixelFormat.B8G8R8A8UIntNormalized,
                    2,
                    monSize);
                _framePool.FrameArrived += OnFrameArrived;
                DebugLog.Write("[WGC] FramePool criado | FrameArrived registrado");
                RecordingPerfProbe.Mark("wgc-framepool-end", $"segment={segmentIndex}");

                _session = _framePool.CreateCaptureSession(_item);
                _session.IsCursorCaptureEnabled = _drawMouse;
                TrySetIsBorderRequired(_session, false);
                DebugLog.Write("[WGC] CaptureSession criada");
                _session.StartCapture();
                DebugLog.Write("[WGC] StartCapture() OK — aguardando frames...");

                RecordingPerfProbe.Mark("wgc-start-end", $"segment={segmentIndex} crop={_cropW}x{_cropH}");
            }
            catch (Exception ex)
            {
                DebugLog.Write($"[WGC] StartSegment({segmentIndex}) EXCEPTION:\n" + ex);
                throw;
            }
        }

        public void StopSegment()
        {
            DebugLog.Write($"[WGC] ═══ StopSegment({_segmentIndex}) begin | framesWritten={_framesWritten} | frameErrors={_frameErrors} ═══");
            _stopping = true;
            RecordingPerfProbe.Mark("wgc-stop-begin", $"segment={_segmentIndex} framesWritten={_framesWritten}");

            try
            {
                if (_framePool != null)
                    _framePool.FrameArrived -= OnFrameArrived;
            }
            catch (Exception ex) { DebugLog.Write("[WGC] StopSegment() detach warning: " + ex.Message); }

            bool entered = false;
            try
            {
                Monitor.Enter(_frameLock, ref entered);
                DebugLog.Write("[WGC] StopSegment() frame lock acquired");
            }
            catch (Exception ex) { DebugLog.Write("[WGC] StopSegment() frame lock acquire ERROR: " + ex); }
            finally { if (entered) Monitor.Exit(_frameLock); }

            try { _session?.Dispose(); } catch (Exception ex) { DebugLog.Write("[WGC] StopSegment() session: " + ex.Message); }
            try { _framePool?.Dispose(); } catch (Exception ex) { DebugLog.Write("[WGC] StopSegment() framePool: " + ex.Message); }

            _session = null;
            _framePool = null;
            _item = null;

            var sw = System.Diagnostics.Stopwatch.StartNew();
            while (!_encodeQueue.IsEmpty && sw.ElapsedMilliseconds < 600)
                Thread.Sleep(5);
            DebugLog.Write($"[WGC] encode queue drained | waited={sw.ElapsedMilliseconds}ms | remaining={_encodeQueue.Count}");

            RecordingPerfProbe.Mark("wgc-mf-stop-begin", $"segment={_segmentIndex}");
            _lastPauseTimestampHns = _mf?.CurrentTimestamp ?? 0L;
            try
            {
                _mf?.PauseSegment();
                DebugLog.Write($"[WGC] StopSegment() writer pausado OK | pauseTs={_lastPauseTimestampHns}");
            }
            catch (Exception ex) { DebugLog.Write("[WGC] StopSegment() writer PauseSegment ERROR: " + ex); }
            RecordingPerfProbe.Mark("wgc-mf-stop-end", $"segment={_segmentIndex}");

            DebugLog.Write($"[WGC] StopSegment({_segmentIndex}) end");
            RecordingPerfProbe.Mark("wgc-stop-end", $"segment={_segmentIndex}");
        }

        public void ReleaseSession()
        {
            DebugLog.Write("[WGC] ReleaseSession begin");
            RecordingPerfProbe.Mark("wgc-session-release-begin");

            _encodeThreadRunning = false;
            _encodeSignal.Release();
            _encodeThread?.Join(2000);
            _encodeThread = null;
            DebugLog.Write("[WGC] encode thread stopped");

            while (_bufferPool.TryTake(out _)) { }
            while (_encodeQueue.TryDequeue(out _)) { }

            if (_frameBuffer != null)
            {
                ArrayPool<byte>.Shared.Return(_frameBuffer);
                _frameBuffer = null;
            }

            var mfLocal = _mf;
            _mf = null;

            if (mfLocal != null)
            {
                try
                {
                    mfLocal.Stop();
                    DebugLog.Write("[WGC] NessMuxerWriter Stop OK");
                }
                catch (Exception ex)
                {
                    DebugLog.Write("[WGC] NessMuxerWriter Stop ERROR: " + ex);
                }
            }

            RecordingPerfProbe.Mark("wgc-staging-release-begin", "session-release");
            if (_stagingTexPtr != IntPtr.Zero) { Marshal.Release(_stagingTexPtr); _stagingTexPtr = IntPtr.Zero; }
            RecordingPerfProbe.Mark("wgc-staging-release-end", "session-release");

            RecordingPerfProbe.Mark("wgc-d3d11-release-begin", "session-release");
            _wgcDevice = null;
            if (_d3d11ContextPtr != IntPtr.Zero) { Marshal.Release(_d3d11ContextPtr); _d3d11ContextPtr = IntPtr.Zero; }
            if (_d3d11DevicePtr != IntPtr.Zero) { Marshal.Release(_d3d11DevicePtr); _d3d11DevicePtr = IntPtr.Zero; }
            RecordingPerfProbe.Mark("wgc-d3d11-release-end", "session-release");

            _sessionInitialized = false;
            RecordingPerfProbe.Mark("wgc-session-release-end");
            DebugLog.Write("[WGC] ReleaseSession end");
        }

        private void EncodeThreadProc()
        {
            DebugLog.Write("[WGC-ENC] encode thread running");
            while (_encodeThreadRunning || !_encodeQueue.IsEmpty)
            {
                if (!_encodeSignal.Wait(50))
                    continue;

                if (!_encodeQueue.TryDequeue(out var item))
                    continue;

                try
                {
                    var mfLocal = _mf;
                    if (mfLocal == null)
                    {
                        _bufferPool.Add(item.Buffer);
                        continue;
                    }

                    mfLocal.WriteFramePts(new ReadOnlySpan<byte>(item.Buffer, 0, item.Buffer.Length), item.PtsHns);

                    if (item.IsFirst && item.SegIdx > 1 && item.PauseHns > 0)
                    {
                        long resumeTs = mfLocal.CurrentTimestamp;
                        lock (_pauseIntervals)
                        {
                            _pauseIntervals.Add((item.PauseHns, resumeTs));
                        }
                        DebugLog.Write($"[WGC] P3.2 PauseInterval completed | pause={item.PauseHns} resume={resumeTs} delta={resumeTs - item.PauseHns}");
                        RecordingPerfProbe.Mark("wgc-p32-resume-recorded", $"pause={item.PauseHns} resume={resumeTs}");
                    }

                    if (item.IsFirst)
                    {
                        _firstStableFrameAtUtc = DateTime.UtcNow;
                        double stableAfterMs = (_firstStableFrameAtUtc.Value - item.CaptureStart).TotalMilliseconds;
                        DebugLog.Write(
                            $"[WGC] PRIMEIRO FRAME ESTÁVEL ESCRITO | crop={_cropW}x{_cropH} | " +
                            $"rowPitch={item.RowPitch} | warmupDiscarded={item.WarmupDiscarded} | " +
                            $"stableAfter={stableAfterMs:F0}ms");
                    }
                }
                catch (Exception ex)
                {
                    DebugLog.Write($"[WGC-ENC] WriteFrame ERROR: {ex.Message}");
                }
                finally
                {
                    _bufferPool.Add(item.Buffer);
                }
            }
            DebugLog.Write("[WGC-ENC] encode thread exiting");
        }

        private void OnFrameArrived(Direct3D11CaptureFramePool sender, object args)
        {
            if (_stopping) return;

            if (!Monitor.TryEnter(_frameLock))
                return;

            try
            {
                if (_stopping) return;

                using var frame = sender.TryGetNextFrame();
                if (frame == null)
                    return;

                IntPtr srcTexPtr = GetD3D11Texture2DFromSurface(frame.Surface);
                if (srcTexPtr == IntPtr.Zero)
                {
                    _frameErrors++;
                    DebugLog.Write($"[WGC] OnFrameArrived: surface→texture falhou (erro #{_frameErrors})");
                    return;
                }

                try
                {
                    var csize = frame.ContentSize;
                    EnsureStagingTexture(csize.Width, csize.Height);

                    D3D11CopyResource(_d3d11ContextPtr, _stagingTexPtr, srcTexPtr);

                    int hr = D3D11Map(_d3d11ContextPtr, _stagingTexPtr, 0, D3D11_MAP_READ, 0, out var mapped);

                    if (hr < 0)
                    {
                        _frameErrors++;
                        DebugLog.Write($"[WGC] D3D11Map FALHOU hr=0x{hr:X8} (erro #{_frameErrors})");
                        return;
                    }

                    try
                    {
                        CopyCropFromMapped(
                            mapped.pData,
                            mapped.RowPitch,
                            _cropX,
                            _cropY,
                            _cropW,
                            _cropH,
                            _frameBuffer);

                        var elapsedMs = (DateTime.UtcNow - _captureStartedAtUtc).TotalMilliseconds;
                        if (elapsedMs < _warmupMilliseconds)
                        {
                            _warmupDiscardedFrames++;
                            if (_warmupDiscardedFrames == 1 || _warmupDiscardedFrames % 10 == 0)
                                DebugLog.Write($"[WGC] warmup discard #{_warmupDiscardedFrames} | elapsed={elapsedMs:F0}ms | target={_warmupMilliseconds}ms");
                            return;
                        }

                        if (_mf == null) return;

                        bool isFirst = !_loggedFirstFrame;
                        if (isFirst)
                        {
                            _loggedFirstFrame = true;
                            _segmentFirstFrameTime = DateTime.UtcNow;
                            _segmentBasePts = _lastPauseTimestampHns + (10_000_000L / _fps);
                        }

                        long elapsedHns = (long)(DateTime.UtcNow - _segmentFirstFrameTime).TotalMilliseconds * 10_000L;
                        long ptsHns = _segmentBasePts + elapsedHns;

                        if (!_bufferPool.TryTake(out byte[] encBuf))
                            encBuf = new byte[_nv12BytesPerFrame];

                        ConvertBgraToNv12(_frameBuffer, _cropW, _cropH, encBuf);

                        while (_encodeQueue.Count >= MaxQueueSize)
                        {
                            if (_encodeQueue.TryDequeue(out var dropped))
                            {
                                _bufferPool.Add(dropped.Buffer);
                                DebugLog.Write($"[WGC] frame drop (queue full) | queueSize={_encodeQueue.Count}");
                            }
                        }

                        var queued = new QueuedFrame(
                            encBuf, isFirst, _segmentIndex, _lastPauseTimestampHns,
                            _captureStartedAtUtc, _warmupDiscardedFrames, mapped.RowPitch, ptsHns);

                        _encodeQueue.Enqueue(queued);
                        _encodeSignal.Release();

                        _framesWritten++;
                        if (_framesWritten % 30 == 0)
                            DebugLog.Write($"[WGC] progress: framesWritten={_framesWritten} erros={_frameErrors} warmupDiscarded={_warmupDiscardedFrames}");
                    }
                    finally
                    {
                        D3D11Unmap(_d3d11ContextPtr, _stagingTexPtr, 0);
                    }
                }
                finally
                {
                    Marshal.Release(srcTexPtr);
                }
            }
            catch (Exception ex)
            {
                _frameErrors++;
                DebugLog.Write($"[WGC] OnFrameArrived EXCEPTION #{_frameErrors}:\n{ex}");
            }
            finally
            {
                Monitor.Exit(_frameLock);
            }
        }

        private IntPtr GetD3D11Texture2DFromSurface(IDirect3DSurface surface)
        {
            if (surface == null)
                return IntPtr.Zero;
            try
            {
                IDirect3DDxgiInterfaceAccess access;
                try { access = surface.As<IDirect3DDxgiInterfaceAccess>(); }
                catch (Exception ex) { DebugLog.Write("[WGC] surface.As ERROR: " + ex); return IntPtr.Zero; }
                IntPtr texPtr = IntPtr.Zero;
                try
                {
                    Guid iidTexture2D = IID_ID3D11Texture2D;
                    texPtr = access.GetInterface(ref iidTexture2D);
                }
                catch (Exception ex) { DebugLog.Write("[WGC] GetInterface ERROR: " + ex); return IntPtr.Zero; }
                if (texPtr == IntPtr.Zero) { DebugLog.Write("[WGC] GetInterface retornou zero"); return IntPtr.Zero; }
                return texPtr;
            }
            catch (Exception ex) { DebugLog.Write("[WGC] GetD3D11Texture2DFromSurface EXCEPTION: " + ex); return IntPtr.Zero; }
        }

        private void EnsureStagingTexture(int width, int height)
        {
            if (_stagingTexPtr != IntPtr.Zero && _stagingW == width && _stagingH == height)
                return;
            if (_stagingTexPtr != IntPtr.Zero) { Marshal.Release(_stagingTexPtr); _stagingTexPtr = IntPtr.Zero; }
            _stagingW = width;
            _stagingH = height;
            var desc = new D3D11_TEXTURE2D_DESC
            {
                Width = (uint)width,
                Height = (uint)height,
                MipLevels = 1,
                ArraySize = 1,
                Format = DXGI_FORMAT_B8G8R8A8_UNORM,
                SampleDesc_Count = 1,
                Usage = D3D11_USAGE_STAGING,
                CPUAccessFlags = D3D11_CPU_ACCESS_READ,
            };
            int hr = D3D11CreateTexture2D_Vtbl(_d3d11DevicePtr, ref desc, IntPtr.Zero, out _stagingTexPtr);
            DebugLog.Write($"[WGC] EnsureStagingTexture {width}x{height} hr=0x{hr:X8} ptr=0x{_stagingTexPtr.ToInt64():X}");
            if (hr < 0 || _stagingTexPtr == IntPtr.Zero)
                Marshal.ThrowExceptionForHR(hr);
        }

        private static GraphicsCaptureItem CreateItemForMonitor(IntPtr hmon)
        {
            DebugLog.Write($"[WGC] CreateItemForMonitor hmon=0x{hmon.ToInt64():X}");
            IntPtr itemPtr = IntPtr.Zero;
            try
            {
                var interop = GraphicsCaptureItem.As<IGraphicsCaptureItemInterop>();
                Guid iid = IID_IGraphicsCaptureItem;
                int hr = interop.CreateForMonitor(hmon, ref iid, out itemPtr);
                DebugLog.Write($"[WGC] CreateForMonitor hr=0x{hr:X8} ptr=0x{itemPtr.ToInt64():X}");
                if (hr < 0) Marshal.ThrowExceptionForHR(hr);
                if (itemPtr == IntPtr.Zero) throw new InvalidOperationException("CreateForMonitor retornou null.");
                var item = WinRT.MarshalInterface<GraphicsCaptureItem>.FromAbi(itemPtr);
                DebugLog.Write("[WGC] GraphicsCaptureItem.FromAbi OK");
                return item;
            }
            catch (Exception ex) { DebugLog.Write("[WGC] CreateItemForMonitor EXCEPTION: " + ex); throw; }
            finally { if (itemPtr != IntPtr.Zero) Marshal.Release(itemPtr); }
        }

        private static System.Drawing.Rectangle GetAbsoluteCaptureRect(ScreenRegion region)
        {
            if (region.CropGdi.HasValue) return region.CropGdi.Value;
            if (region.SelectedScreen != null) return region.SelectedScreen.Bounds;
            throw new InvalidOperationException("Nenhum alvo de captura selecionado.");
        }

        private static void TrySetIsBorderRequired(GraphicsCaptureSession session, bool value)
        {
            try { var p = typeof(GraphicsCaptureSession).GetProperty("IsBorderRequired"); if (p?.CanWrite == true) p.SetValue(session, value); } catch { }
        }

        private static void MakeEven(ref int w, ref int h)
        {
            if ((w & 1) == 1) w--;
            if ((h & 1) == 1) h--;
            if (w < 16) w = 16;
            if (h < 16) h = 16;
        }

        private static void ConvertBgraToNv12(byte[] bgra, int width, int height, byte[] destinationNv12)
        {
            if (bgra == null) throw new ArgumentNullException(nameof(bgra));
            if (destinationNv12 == null) throw new ArgumentNullException(nameof(destinationNv12));

            int ySize = width * height;
            if (destinationNv12.Length < ySize + (ySize / 2))
                throw new ArgumentException("destinationNv12 menor que o esperado.", nameof(destinationNv12));

            for (int row = 0; row < height; row++)
            {
                int srcRow = row * width * 4;
                int dstRow = row * width;
                for (int col = 0; col < width; col++)
                {
                    int s = srcRow + col * 4;
                    byte b = bgra[s]; byte g = bgra[s + 1]; byte r = bgra[s + 2];
                    destinationNv12[dstRow + col] = (byte)Math.Clamp(((66 * r + 129 * g + 25 * b + 128) >> 8) + 16, 16, 235);
                }
            }

            int uvBase = ySize;
            for (int row = 0; row < height; row += 2)
            {
                int srcRow = row * width * 4;
                int dstUv = uvBase + (row / 2) * width;
                for (int col = 0; col < width; col += 2)
                {
                    int s = srcRow + col * 4;
                    byte b = bgra[s]; byte g = bgra[s + 1]; byte r = bgra[s + 2];
                    destinationNv12[dstUv + col] = (byte)Math.Clamp(((-38 * r - 74 * g + 112 * b + 128) >> 8) + 128, 16, 240);
                    destinationNv12[dstUv + col + 1] = (byte)Math.Clamp(((112 * r - 94 * g - 18 * b + 128) >> 8) + 128, 16, 240);
                }
            }
        }

        public int ScreenWidth => _cropW;
        public int ScreenHeight => _cropH;
        public int ScreenFps => _fps;
        public long ScreenFrameCount => _mf?.FrameCount ?? 0L;
        public int ScreenStrideY => _mf?.StrideY ?? _cropW;
        public int ScreenStrideUV => _mf?.StrideUV ?? _cropW;
        public string ScreenPixelFormat => "h264";
        public bool IsScreenRawIntermediate => false;

        public IReadOnlyList<(long PauseHns, long ResumeHns)> PauseIntervals
        {
            get { lock (_pauseIntervals) { return _pauseIntervals.ToArray(); } }
        }

        public string ScreenContinuousFile => _paths.ScreenContinuous();

        public void Dispose()
        {
            if (_disposed) return;
            _disposed = true;
            if (_sessionInitialized)
            {
                try { StopSegment(); } catch { }
                ReleaseSession();
            }
        }

        private const int D3D11_SDK_VERSION = 7;
        private const uint D3D11_CREATE_DEVICE_BGRA_SUPPORT = 0x20;
        private const int D3D_DRIVER_TYPE_HARDWARE = 1;
        private const uint DXGI_FORMAT_B8G8R8A8_UNORM = 87;
        private const uint D3D11_USAGE_STAGING = 3;
        private const uint D3D11_CPU_ACCESS_READ = 0x20000;
        private const int D3D11_MAP_READ = 1;

        [DllImport("d3d11.dll", CallingConvention = CallingConvention.StdCall)]
        private static extern int D3D11CreateDevice(IntPtr pAdapter, int driverType, IntPtr software, uint flags, int[] pFeatureLevels, int featureLevels, int sdkVersion, out IntPtr ppDevice, out int pFeatureLevel, out IntPtr ppImmediateContext);

        [DllImport("d3d11.dll", EntryPoint = "CreateDirect3D11DeviceFromDXGIDevice", CallingConvention = CallingConvention.StdCall)]
        private static extern int CreateDirect3D11DeviceFromDXGIDevice(IntPtr dxgiDevice, out IntPtr graphicsDevice);

        private static int D3D11CreateTexture2D_Vtbl(IntPtr dev, ref D3D11_TEXTURE2D_DESC desc, IntPtr pData, out IntPtr tex)
        {
            IntPtr fnPtr = Marshal.ReadIntPtr(Marshal.ReadIntPtr(dev), IntPtr.Size * 5);
            return Marshal.GetDelegateForFunctionPointer<CreateTexture2DDelegate>(fnPtr)(dev, ref desc, pData, out tex);
        }

        [UnmanagedFunctionPointer(CallingConvention.StdCall)]
        private delegate int CreateTexture2DDelegate(IntPtr self, ref D3D11_TEXTURE2D_DESC desc, IntPtr pData, out IntPtr ppTex);

        private static void D3D11CopyResource(IntPtr ctx, IntPtr dst, IntPtr src)
        {
            IntPtr fnPtr = Marshal.ReadIntPtr(Marshal.ReadIntPtr(ctx), IntPtr.Size * 47);
            Marshal.GetDelegateForFunctionPointer<CopyResourceDelegate>(fnPtr)(ctx, dst, src);
        }

        [UnmanagedFunctionPointer(CallingConvention.StdCall)]
        private delegate void CopyResourceDelegate(IntPtr self, IntPtr dst, IntPtr src);

        private static int D3D11Map(IntPtr ctx, IntPtr res, int sub, int mapType, int flags, out D3D11_MAPPED_SUBRESOURCE mapped)
        {
            IntPtr fnPtr = Marshal.ReadIntPtr(Marshal.ReadIntPtr(ctx), IntPtr.Size * 14);
            return Marshal.GetDelegateForFunctionPointer<MapDelegate>(fnPtr)(ctx, res, sub, mapType, flags, out mapped);
        }

        [UnmanagedFunctionPointer(CallingConvention.StdCall)]
        private delegate int MapDelegate(IntPtr self, IntPtr res, int sub, int mapType, int flags, out D3D11_MAPPED_SUBRESOURCE mapped);

        private static void D3D11Unmap(IntPtr ctx, IntPtr res, int sub)
        {
            IntPtr fnPtr = Marshal.ReadIntPtr(Marshal.ReadIntPtr(ctx), IntPtr.Size * 15);
            Marshal.GetDelegateForFunctionPointer<UnmapDelegate>(fnPtr)(ctx, res, sub);
        }

        [UnmanagedFunctionPointer(CallingConvention.StdCall)]
        private delegate void UnmapDelegate(IntPtr self, IntPtr res, int sub);

        private static unsafe void CopyCropFromMapped(IntPtr pData, int rowPitch, int x, int y, int w, int h, byte[] dst)
        {
            int rowBytes = w * 4;
            int dstOff = 0;
            byte* basePtr = (byte*)pData.ToPointer();
            for (int row = 0; row < h; row++)
            {
                byte* srcRow = basePtr + ((y + row) * rowPitch) + (x * 4);
                new ReadOnlySpan<byte>(srcRow, rowBytes).CopyTo(new Span<byte>(dst, dstOff, rowBytes));
                dstOff += rowBytes;
            }
        }

        [StructLayout(LayoutKind.Sequential)]
        private struct D3D11_TEXTURE2D_DESC { public uint Width, Height, MipLevels, ArraySize, Format, SampleDesc_Count, SampleDesc_Quality, Usage, BindFlags, CPUAccessFlags, MiscFlags; }

        [StructLayout(LayoutKind.Sequential)]
        private struct D3D11_MAPPED_SUBRESOURCE { public IntPtr pData; public int RowPitch, DepthPitch; }

        [StructLayout(LayoutKind.Sequential)]
        private struct RECT { public int Left, Top, Right, Bottom; }

        [StructLayout(LayoutKind.Sequential, CharSet = CharSet.Auto)]
        private struct MONITORINFOEX { public int cbSize; public RECT rcMonitor, rcWork; public int dwFlags; [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 32)] public string szDevice; }

        private readonly struct MonitorRect
        {
            public int Left { get; }
            public int Top { get; }
            public int Width { get; }
            public int Height { get; }
            public MonitorRect(int l, int t, int w, int h) { Left = l; Top = t; Width = w; Height = h; }
        }

        [DllImport("user32.dll")]
        private static extern IntPtr MonitorFromPoint(System.Drawing.Point pt, uint flags);

        [DllImport("user32.dll", CharSet = CharSet.Auto)]
        private static extern bool GetMonitorInfo(IntPtr hMon, ref MONITORINFOEX lpmi);

        private static bool TryGetMonitorRect(IntPtr hmon, out MonitorRect rect)
        {
            var mi = new MONITORINFOEX { cbSize = Marshal.SizeOf<MONITORINFOEX>() };
            if (!GetMonitorInfo(hmon, ref mi)) { rect = default; return false; }
            rect = new MonitorRect(mi.rcMonitor.Left, mi.rcMonitor.Top, mi.rcMonitor.Right - mi.rcMonitor.Left, mi.rcMonitor.Bottom - mi.rcMonitor.Top);
            return true;
        }

        [ComImport, Guid("A9B3D012-3DF2-4EE3-B8D1-8695F457D3C1"), InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
        private interface IDirect3DDxgiInterfaceAccess { IntPtr GetInterface([In] ref Guid iid); }

        [ComImport, Guid("54EC77FA-1377-44E6-8C32-88FD5F44C84C"), InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
        private interface IDXGIDevice { }

        [ComImport, Guid("3628E81B-3CAC-4C60-B7F4-23CE0E0C3356"), InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
        private interface IGraphicsCaptureItemInterop
        {
            [PreserveSig] int CreateForWindow(IntPtr window, ref Guid iid, out IntPtr result);
            [PreserveSig] int CreateForMonitor(IntPtr monitor, ref Guid iid, out IntPtr result);
        }
    }
}
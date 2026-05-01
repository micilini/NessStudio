using System;
using System.IO;
using System.Runtime.InteropServices;
using NessStudio.ViewModel.Helpers;

namespace NessStudio.Recording.Windows
{
    
    
    
    
    
    
    
    
    
    public sealed class NessMuxerWriter : IDisposable
    {
        private readonly object _sync = new();
        private IntPtr _muxer;
        private string _outputPath;
        private int _width;
        private int _height;
        private int _fps;
        private int _frameSize;
        private long _rtStep;
        private long _currentTimestamp;
        private long _frameCount;
        private bool _started;
        private bool _disposed;

        

        public int Width => _width;
        public int Height => _height;
        public int Fps => _fps;

        
        
        
        
        public int StrideY => _width;
        public int StrideUV => _width;
        public long FrameCount => _frameCount;

        public long CurrentTimestamp
        {
            get
            {
                lock (_sync)
                    return _currentTimestamp;
            }
        }

        
        
        
        
        
        
        
        public void Start(string outputPath, int width, int height, int fps)
        {
            lock (_sync)
            {
                StopInternal(resetTimestamp: true);

                if (string.IsNullOrWhiteSpace(outputPath))
                    throw new ArgumentNullException(nameof(outputPath));
                if (width <= 0)
                    throw new ArgumentOutOfRangeException(nameof(width));
                if (height <= 0)
                    throw new ArgumentOutOfRangeException(nameof(height));
                if (fps <= 0)
                    throw new ArgumentOutOfRangeException(nameof(fps));
                if ((width & 1) != 0 || (height & 1) != 0)
                    throw new ArgumentException("NV12 exige dimensões pares.");

                Directory.CreateDirectory(Path.GetDirectoryName(outputPath)!);

                _outputPath = outputPath;
                _width = width;
                _height = height;
                _fps = fps;
                _frameSize = (width * height * 3) / 2;
                _rtStep = 10_000_000L / fps;
                _frameCount = 0;
                _currentTimestamp = 0;

                
                int bitrate = CalculateBitrate(width, height);

                var config = new NessMuxerInterop.NessMuxerConfig
                {
                    output_path = outputPath,
                    width = width,
                    height = height,
                    fps = fps,
                    bitrate_kbps = bitrate,
                    encoder_type = NessMuxerInterop.NESS_ENCODER_AUTO
                };

                int ret = NessMuxerInterop.ness_muxer_open(out _muxer, ref config);
                if (ret != NessMuxerInterop.NESS_OK)
                {
                    string err = _muxer != IntPtr.Zero
                        ? NessMuxerInterop.GetError(_muxer)
                        : "unknown";
                    _muxer = IntPtr.Zero;
                    throw new InvalidOperationException(
                        $"NessMuxer open failed (ret={ret}): {err}");
                }

                _started = true;
                DebugLog.Write(
                    $"[NessMuxerWriter] Start OK | file={_outputPath} | {_width}x{_height} @{_fps}fps | bitrate={bitrate}kbps");
            }
        }

        
        
        
        
        public void WriteFrame(ReadOnlySpan<byte> nv12Frame)
        {
            lock (_sync)
            {
                if (!_started || _muxer == IntPtr.Zero)
                    return;

                if (nv12Frame.Length < _frameSize)
                    throw new ArgumentException(
                        $"Frame NV12 menor que o esperado. expected={_frameSize} actual={nv12Frame.Length}",
                        nameof(nv12Frame));

                int ret;

                unsafe
                {
                    fixed (byte* ptr = nv12Frame)
                    {
                        ret = NessMuxerInterop.ness_muxer_write_frame(
                            _muxer, (IntPtr)ptr, _frameSize);
                    }
                }

                if (ret != NessMuxerInterop.NESS_OK)
                {
                    string err = NessMuxerInterop.GetError(_muxer);
                    DebugLog.Write(
                        $"[NessMuxerWriter] WriteFrame FAILED at frame {_frameCount} (ret={ret}): {err}");
                    return;
                }

                _frameCount++;
                _currentTimestamp += _rtStep;

                if (_frameCount == 1)
                {
                    DebugLog.Write(
                        $"[NessMuxerWriter] PRIMEIRO FRAME gravado | ts={_currentTimestamp} bytes={_frameSize}");
                }
                else if ((_frameCount % 30) == 0)
                {
                    DebugLog.Write(
                        $"[NessMuxerWriter] progress | frames={_frameCount} | ts={_currentTimestamp}");
                }
            }
        }


        public void WriteFramePts(ReadOnlySpan<byte> nv12Frame, long ptsHns)
        {
            lock (_sync)
            {
                if (!_started || _muxer == IntPtr.Zero)
                    return;

                if (nv12Frame.Length < _frameSize)
                    throw new ArgumentException(
                        $"Frame NV12 menor que o esperado. expected={_frameSize} actual={nv12Frame.Length}",
                        nameof(nv12Frame));

                int ret;

                unsafe
                {
                    fixed (byte* ptr = nv12Frame)
                    {
                        ret = NessMuxerInterop.ness_muxer_write_frame_pts(
                            _muxer, (IntPtr)ptr, _frameSize, ptsHns);
                    }
                }

                if (ret != NessMuxerInterop.NESS_OK)
                {
                    string err = NessMuxerInterop.GetError(_muxer);
                    DebugLog.Write(
                        $"[NessMuxerWriter] WriteFramePts FAILED at frame {_frameCount} pts={ptsHns} (ret={ret}): {err}");
                    return;
                }

                _frameCount++;
                _currentTimestamp = ptsHns;

                if (_frameCount == 1)
                    DebugLog.Write($"[NessMuxerWriter] PRIMEIRO FRAME gravado | pts={ptsHns} bytes={_frameSize}");
                else if ((_frameCount % 30) == 0)
                    DebugLog.Write($"[NessMuxerWriter] progress | frames={_frameCount} | pts={ptsHns}");
            }
        }



        public void PauseSegment()
        {
            lock (_sync)
            {
                if (!_started || _muxer == IntPtr.Zero)
                    return;

                DebugLog.Write(
                    $"[NessMuxerWriter] PauseSegment OK | ts={_currentTimestamp} | frames={_frameCount}");
            }
        }

        public void Stop()
        {
            lock (_sync)
            {
                StopInternal(resetTimestamp: false);
            }
        }

        public void ResetTimestamp()
        {
            lock (_sync)
            {
                _currentTimestamp = 0;
            }
        }

        public void Dispose()
        {
            if (_disposed)
                return;

            lock (_sync)
            {
                if (_disposed)
                    return;

                StopInternal(resetTimestamp: false);
                _disposed = true;
            }
        }

        private void StopInternal(bool resetTimestamp)
        {
            if (_muxer != IntPtr.Zero)
            {
                try
                {
                    int ret = NessMuxerInterop.ness_muxer_close(_muxer);
                    if (ret != NessMuxerInterop.NESS_OK)
                    {
                        DebugLog.Write(
                            $"[NessMuxerWriter] close returned {ret}");
                    }
                }
                catch (Exception ex)
                {
                    DebugLog.Write(
                        $"[NessMuxerWriter] close exception: {ex.Message}");
                }

                
                
                _muxer = IntPtr.Zero;
            }

            if (_started)
            {
                DebugLog.Write(
                    $"[NessMuxerWriter] Stop OK | file={_outputPath} | frames={_frameCount} | ts={_currentTimestamp}");
            }

            _outputPath = null;
            _width = 0;
            _height = 0;
            _fps = 0;
            _frameSize = 0;
            _frameCount = 0;
            _started = false;

            if (resetTimestamp)
                _currentTimestamp = 0;
        }

        
        
        
        
        private static int CalculateBitrate(int width, int height)
        {
            long pixels = (long)width * height;

            if (pixels >= 3840 * 2160) return 12000;  
            if (pixels >= 2560 * 1440) return 8000;   
            if (pixels >= 1920 * 1080) return 6000;   
            if (pixels >= 1280 * 720) return 4000;   
            if (pixels >= 640 * 480) return 2000;    
            return 1000;                                
        }
    }
}
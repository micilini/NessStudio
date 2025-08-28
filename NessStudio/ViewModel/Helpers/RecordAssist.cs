using NAudio.CoreAudioApi;
using NAudio.Wave;
using System;
using System.Diagnostics;
using System.IO;
using System.Linq;
using System.Runtime.InteropServices;
using System.Threading.Tasks;
using System.Windows;
using System.Windows.Forms;
using System.Collections.Generic;
using System.Text;
using NessStudio.Models;

namespace NessStudio.ViewModel.Helpers
{
    public class RecordAssist : IDisposable
    {
        private readonly NessStudio.Models.RecordingOutputPaths _paths;
        private readonly RecordingTargets _targets;

        private NessStudio.Models.ScreenRegion _region;

        private Process _ffScreen;
        private Process _ffWebcam;

        private WasapiCapture _micCapture;
        private WaveFileWriter _micWriter;

        private WasapiLoopbackCapture _loopCapture;
        private WaveFileWriter _loopWriter;

        private readonly NessStudio.Models.RecordingSegmentState _seg = new NessStudio.Models.RecordingSegmentState();

        private System.Timers.Timer _loopTick;
        private NessStudio.Models.AudioClockState _clock = new NessStudio.Models.AudioClockState();

        private readonly NessStudio.Models.FfmpegSettings _ff;

        public RecordAssist(RecordingOutputPaths paths,
                    RecordingTargets targets,
                    System.Windows.Rect? cropPx = null,
                    FfmpegSettings settings = null)
        {
            _paths = paths ?? throw new ArgumentNullException(nameof(paths));
            _targets = targets ?? throw new ArgumentNullException(nameof(targets));
            _region = new ScreenRegion(targets.Screen, cropPx);

            _ff = settings ?? FfmpegSettings.CreateDefault();

            if (!File.Exists(_ff.FfmpegExePath))
                throw new FileNotFoundException($"ffmpeg.exe not found at: {_ff.FfmpegExePath}");
        }

        public async Task StartAsync()
        {
            if (_seg.IsRunning) return;

            _seg.Start();

            StartScreenSegment();
            StartWebcamSegment();
            StartMicSegment();
            StartLoopbackSegment();

            await Task.Delay(3000);
        }

        public async Task PauseAsync()
        {
            if (!_seg.IsRunning || _seg.IsPaused) return;
            _seg.TryPause();

            await Task.Run(() =>
            {
                StopScreenSegment();
                StopWebcamSegment();
                StopMicSegment();
                StopLoopbackSegment();
            });
        }
        public async Task ResumeAsync()
        {
            if (!_seg.IsRunning || !_seg.IsPaused) return;
            _seg.TryResume();

            await Task.Run(() =>
            {
                StartScreenSegment();
                StartWebcamSegment();
                StartMicSegment();
                StartLoopbackSegment();
            });
        }

        public async Task StopAsync()
        {
            if (!_seg.IsRunning) return;

            await Task.Run(() =>
            {
                StopScreenSegment();
                StopWebcamSegment();
                StopMicSegment();
                StopLoopbackSegment();
                _seg.TryStop();
            });
        }

        private void StartScreenSegment()
        {
            _ffScreen = ScreenCaptureService.Start(_ff, _region, _paths, _seg.SegmentIndex);
        }

        private void StopScreenSegment()
        {
            ScreenCaptureService.Stop(ref _ffScreen);
        }

        private void StartWebcamSegment()
        {
            if (string.IsNullOrWhiteSpace(_targets.WebcamName)) return;

            string outFile = _paths.WebcamSegment(_seg.SegmentIndex);
            _ffWebcam = WebcamCaptureService.Start(_ff, _targets.WebcamName, outFile);
        }

        private void StopWebcamSegment()
        {
            WebcamCaptureService.Stop(ref _ffWebcam);
        }

        private void StartMicSegment()
        {
            if (string.IsNullOrWhiteSpace(_targets.MicDeviceId)) return;

            string outFile = _paths.MicSegment(_seg.SegmentIndex);
            (_micCapture, _micWriter) = MicCaptureService.Start(_targets.MicDeviceId, outFile);
        }

        private void StopMicSegment()
        {
            MicCaptureService.Stop(_micCapture, _micWriter);
            _micCapture = null;
            _micWriter = null;
        }

        private void StartLoopbackSegment()
        {
            if (string.IsNullOrWhiteSpace(_targets.LoopbackDeviceId)) return;

            string outFile = _paths.SystemSegment(_seg.SegmentIndex);

            (_loopCapture, _loopWriter, _clock, _loopTick) =
                SystemLoopbackService.Start(_targets.LoopbackDeviceId, outFile);
        }

        private void StopLoopbackSegment()
        {
            SystemLoopbackService.Stop(_loopCapture, _loopWriter, _loopTick);

            _loopTick = null;
            _loopWriter = null;
            _loopCapture = null;
            _clock = new NessStudio.Models.AudioClockState();
        }

        public async Task<string> StopAndFinalizeAsync()
        {
            if (_seg.IsRunning)
            {
                await Task.Run(() =>
                {
                    StopScreenSegment();
                    StopWebcamSegment();
                    StopMicSegment();
                    StopLoopbackSegment();
                    _seg.TryStop();
                });
            }

            string screenOut = await FinalizeTrackAsync("screen", ".mp4");
            string webcamOut = await FinalizeTrackAsync("webcam", ".mp4");
            string micOut = await FinalizeTrackAsync("mic", ".wav");
            string sysOut = await FinalizeTrackAsync("system", ".wav");

            return screenOut
                ?? webcamOut
                ?? micOut
                ?? sysOut;
        }

        private async Task<string> FinalizeTrackAsync(string prefix, string ext)
        {
            var plan = TrackFinalizer.BuildPlan(_paths, prefix, ext);
            return await TrackFinalizer.ConcatIfNeededAsync(_ff.FfmpegExePath, plan);
        }

        public void Dispose()
        {
            StopScreenSegment();
            StopWebcamSegment();
            StopMicSegment();
            StopLoopbackSegment();
        }
    }
}
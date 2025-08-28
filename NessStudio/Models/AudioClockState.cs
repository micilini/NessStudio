using NAudio.Wave;
using System;
using System.Collections.Generic;
using System.Linq;
using System.Text;
using System.Threading.Tasks;

namespace NessStudio.Models
{
    public sealed class AudioClockState
    {
        public DateTime StartUtc { get; private set; }
        public long BytesWritten { get; private set; }
        public WaveFormat Format { get; private set; }
        public bool IsRunning { get; private set; }

        public void Start(WaveFormat format)
        {
            Format = format ?? throw new ArgumentNullException(nameof(format));
            BytesWritten = 0;
            StartUtc = DateTime.UtcNow;
            IsRunning = true;
        }

        public void Stop() => IsRunning = false;

        public void AddWritten(int bytes)
        {
            if (!IsRunning || Format == null || bytes <= 0) return;
            BytesWritten += bytes - (bytes % Format.BlockAlign);
        }

        public int AlignedBytesForMs(int ms)
        {
            if (Format == null || ms <= 0) return 0;
            int bytes = (int)(Format.AverageBytesPerSecond * (ms / 1000.0));
            bytes -= bytes % Format.BlockAlign;
            return bytes;
        }

        public long TargetBytesAt(DateTime utcNow)
        {
            if (!IsRunning || Format == null) return BytesWritten;
            var elapsed = utcNow - StartUtc;
            long target = (long)(Format.AverageBytesPerSecond * elapsed.TotalSeconds);
            target -= target % Format.BlockAlign;
            return target;
        }

        public int MissingBytes(DateTime utcNow, int maxPerTickBytes)
        {
            if (!IsRunning || Format == null) return 0;

            long missing = TargetBytesAt(utcNow) - BytesWritten;
            if (missing <= 0) return 0;

            int toWrite = (int)Math.Min(missing, Math.Max(Format.BlockAlign, maxPerTickBytes));
            toWrite -= toWrite % Format.BlockAlign;
            return toWrite > 0 ? toWrite : 0;
        }
    }
}

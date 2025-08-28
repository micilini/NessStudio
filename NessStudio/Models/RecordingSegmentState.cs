using System;
using System.Collections.Generic;
using System.Linq;
using System.Text;
using System.Threading.Tasks;

namespace NessStudio.Models
{
    public sealed class RecordingSegmentState
    {
        public int SegmentIndex { get; private set; }
        public bool IsRunning { get; private set; }
        public bool IsPaused { get; private set; }
        public DateTime StartedAt { get; private set; }

        public void Start()
        {
            IsRunning = true;
            IsPaused = false;
            SegmentIndex = 1;
            StartedAt = DateTime.UtcNow;
        }

        public bool TryPause()
        {
            if (!IsRunning || IsPaused) return false;
            IsPaused = true;
            return true;
        }

        public bool TryResume()
        {
            if (!IsRunning || !IsPaused) return false;
            IsPaused = false;
            AdvanceSegment();
            return true;
        }

        public void AdvanceSegment()
        {
            if (!IsRunning) return;
            SegmentIndex = Math.Max(1, SegmentIndex + 1);
            StartedAt = DateTime.UtcNow;
        }

        public bool TryStop()
        {
            if (!IsRunning) return false;
            IsRunning = false;
            IsPaused = false;
            return true;
        }
    }
}

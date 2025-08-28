using System;
using System.Collections.Generic;
using System.Linq;
using System.Text;
using System.Threading.Tasks;
using System.Windows.Forms;

namespace NessStudio.Models
{
    public sealed class RecordingTargets
    {
        public Screen Screen { get; }                    
        public string WebcamName { get; }               
        public string MicDeviceId { get; }              
        public string LoopbackDeviceId { get; }         

        public RecordingTargets(
            Screen screen,
            string webcamName,
            string micDeviceId,
            string loopbackDeviceId)
        {
            Screen = screen;
            WebcamName = string.IsNullOrWhiteSpace(webcamName) ? null : webcamName;
            MicDeviceId = string.IsNullOrWhiteSpace(micDeviceId) ? null : micDeviceId;
            LoopbackDeviceId = string.IsNullOrWhiteSpace(loopbackDeviceId) ? null : loopbackDeviceId;
        }

        public bool CaptureScreen => Screen != null;                     
        public bool CaptureWebcam => !string.IsNullOrWhiteSpace(WebcamName);
        public bool CaptureMic => !string.IsNullOrWhiteSpace(MicDeviceId);
        public bool CaptureSystem => !string.IsNullOrWhiteSpace(LoopbackDeviceId);

        public bool AnyVideo => CaptureScreen || CaptureWebcam;
        public bool AnyAudio => CaptureMic || CaptureSystem;

        public bool IsValid(out string message)
        {
            if (!AnyVideo && !AnyAudio)
            {
                message = "No source selected for recording.";
                return false;
            }
            message = null;
            return true;
        }
    }
}

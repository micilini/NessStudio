using System;
using System.Collections.Generic;
using System.Linq;
using System.Text;
using System.Threading.Tasks;
using System.Windows.Forms;

namespace NessStudio.Models
{
    public sealed class RecordingDeviceSelection
    {
        public string WebcamName { get; }
        public string MicDeviceId { get; }
        public string LoopbackDeviceId { get; }
        public string DisplayFriendlyName { get; }

        public RecordingDeviceSelection(
            string webcamName,
            string micDeviceId,
            string loopbackDeviceId,
            string displayFriendlyName)
        {
            WebcamName = Normalize(webcamName);
            MicDeviceId = Normalize(micDeviceId);
            LoopbackDeviceId = Normalize(loopbackDeviceId);
            DisplayFriendlyName = displayFriendlyName?.Trim();
        }

        public bool CaptureWebcam => !string.IsNullOrWhiteSpace(WebcamName);
        public bool CaptureMic => !string.IsNullOrWhiteSpace(MicDeviceId);
        public bool CaptureSystem => !string.IsNullOrWhiteSpace(LoopbackDeviceId);
        public bool AnyVideo => CaptureWebcam;
        public bool AnyAudio => CaptureMic || CaptureSystem;

        public NessStudio.Models.RecordingTargets BuildTargets(Screen screen)
            => new NessStudio.Models.RecordingTargets(
                screen,
                WebcamName,
                MicDeviceId,
                LoopbackDeviceId
            );

        public bool IsValid(out string message)
        {
            if (!AnyVideo && !AnyAudio)
            {
                message = "No capture source was selected.";
                return false;
            }
            message = null;
            return true;
        }

        public override string ToString()
        {
            var sb = new StringBuilder();
            sb.Append("[Devices] ");
            sb.Append(CaptureWebcam ? $"Webcam=\"{WebcamName}\" " : "Webcam=off ");
            sb.Append(CaptureMic ? $"MicID={MicDeviceId} " : "Mic=off ");
            sb.Append(CaptureSystem ? $"LoopID={LoopbackDeviceId} " : "System=off ");
            if (!string.IsNullOrWhiteSpace(DisplayFriendlyName))
                sb.Append($"Display=\"{DisplayFriendlyName}\"");
            return sb.ToString().TrimEnd();
        }

        private static string Normalize(string s)
            => string.IsNullOrWhiteSpace(s) ? null : s.Trim();
    }
}

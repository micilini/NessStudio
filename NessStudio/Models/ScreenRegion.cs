using System;
using System.Collections.Generic;
using System.Linq;
using System.Runtime.InteropServices;
using System.Text;
using System.Threading.Tasks;
using System.Windows.Forms;

namespace NessStudio.Models
{
    public sealed class ScreenRegion
    {
        public Screen SelectedScreen { get; }
        public System.Drawing.Rectangle? CropGdi { get; }
        public bool IsCropped => CropGdi.HasValue;
        public bool ShouldCapture => IsCropped || SelectedScreen != null;

        public ScreenRegion(Screen selectedScreen, System.Windows.Rect? cropPx)
        {
            SelectedScreen = selectedScreen;

            if (cropPx.HasValue)
            {
                var r = cropPx.Value;
                CropGdi = new System.Drawing.Rectangle(
                    (int)Math.Round(r.X),
                    (int)Math.Round(r.Y),
                    (int)Math.Round(r.Width),
                    (int)Math.Round(r.Height)
                );
            }
        }
    }
}

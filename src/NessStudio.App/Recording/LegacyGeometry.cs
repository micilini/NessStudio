using NessStudio.Models;
using System.Windows.Forms;

namespace NessStudio.Recording;

/// <summary>Converts frontend geometry to physical value descriptors at the App boundary.</summary>
public static class LegacyGeometry
{
    /// <summary>Copies the monitor identity and bounds without retaining a Screen object.</summary>
    /// <param name="screen">The optional frontend monitor selection.</param>
    /// <returns>The monitor snapshot, or null.</returns>
    public static MonitorDescriptor? Describe(Screen? screen) => screen is null ? null :
        new MonitorDescriptor(screen.DeviceName,
            new PhysicalRectangle(screen.Bounds.X, screen.Bounds.Y, screen.Bounds.Width, screen.Bounds.Height), screen.Primary);

    /// <summary>Converts a physical-pixel WPF crop using the existing rounding policy.</summary>
    /// <param name="crop">The optional physical-pixel crop; callers resolve DIPs before this boundary.</param>
    /// <returns>The physical crop, or null.</returns>
    public static PhysicalRectangle? Describe(System.Windows.Rect? crop) => crop is { } value ?
        new PhysicalRectangle(checked((int)Math.Round(value.X)), checked((int)Math.Round(value.Y)),
            checked((int)Math.Round(value.Width)), checked((int)Math.Round(value.Height))) : null;

    internal static System.Drawing.Rectangle ToGdi(PhysicalRectangle value) =>
        new(value.X, value.Y, value.Width, value.Height);
}

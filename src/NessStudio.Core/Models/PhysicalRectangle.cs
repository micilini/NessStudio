namespace NessStudio.Models;

/// <summary>Describes a rectangle in physical desktop pixels without UI types.</summary>
public readonly record struct PhysicalRectangle
{
    /// <summary>Initializes a rectangle with a signed origin and positive extent.</summary>
    /// <param name="x">The horizontal desktop coordinate.</param>
    /// <param name="y">The vertical desktop coordinate.</param>
    /// <param name="width">The positive width.</param>
    /// <param name="height">The positive height.</param>
    /// <exception cref="ArgumentOutOfRangeException">An extent is not positive.</exception>
    [System.Text.Json.Serialization.JsonConstructor]
    public PhysicalRectangle(int x, int y, int width, int height)
    {
        ArgumentOutOfRangeException.ThrowIfNegativeOrZero(width);
        ArgumentOutOfRangeException.ThrowIfNegativeOrZero(height);
        X = x; Y = y; Width = width; Height = height;
    }
    /// <summary>Gets the horizontal origin.</summary>
    public int X { get; }
    /// <summary>Gets the vertical origin.</summary>
    public int Y { get; }
    /// <summary>Gets the width in physical pixels.</summary>
    public int Width { get; }
    /// <summary>Gets the height in physical pixels.</summary>
    public int Height { get; }
}

/// <summary>Describes a selection snapshot without retaining a Screen or HMONITOR.</summary>
/// <param name="DeviceName">The current display identity, resolved again when opening the source.</param>
/// <param name="Bounds">The physical bounds at selection time.</param>
/// <param name="IsPrimary">A value indicating the primary monitor at enumeration time.</param>
public sealed record MonitorDescriptor(string DeviceName, PhysicalRectangle Bounds, bool IsPrimary);

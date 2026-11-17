namespace NessStudio.Models;

/// <summary>Describes a source region without WPF, WinForms or GDI dependencies.</summary>
public sealed class ScreenRegion
{
    /// <summary>Initializes a source region from a monitor and optional physical crop.</summary>
    /// <param name="selectedScreen">The optional monitor descriptor.</param>
    /// <param name="crop">The optional physical desktop crop.</param>
    public ScreenRegion(MonitorDescriptor? selectedScreen, PhysicalRectangle? crop)
    { SelectedScreen = selectedScreen; Crop = crop; }
    /// <summary>Gets the optional monitor.</summary>
    public MonitorDescriptor? SelectedScreen { get; }
    /// <summary>Gets the optional physical crop.</summary>
    public PhysicalRectangle? Crop { get; }
    /// <summary>Gets a value indicating whether a crop is selected.</summary>
    public bool IsCropped => Crop.HasValue;
    /// <summary>Gets a value indicating whether a screen source exists.</summary>
    public bool ShouldCapture => IsCropped || SelectedScreen is not null;
}

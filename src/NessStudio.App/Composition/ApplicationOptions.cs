using System.Windows.Interop;
using System.IO;

namespace NessStudio.Composition;

/// <summary>Defines frontend paths, single-instance identity and WPF presentation policy.</summary>
public sealed record ApplicationOptions(string DataDirectory, string DocumentsDirectory,
    string InstanceMutexName, RenderMode PresentationMode = RenderMode.Default)
{
    /// <summary>Gets the app-owned recording workspace; exported files can be saved separately by the user.</summary>
    public string RecordingsDirectory => Path.Combine(DataDirectory, "Recordings");
    /// <summary>Creates the existing per-user paths and single-instance policy.</summary>
    public static ApplicationOptions ForCurrentUser() => new(
        Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData), "NessStudio"),
        Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.MyDocuments), "NessStudio"),
        @"Local\NessStudio");
}

using System.ComponentModel;
using System.Runtime.InteropServices;

namespace NessStudio.Recording.Engines;

/// <summary>Queries usable space on the actual output directory's volume, including mount points and quotas.</summary>
public static class RecordingStorageSpace
{
    [DllImport("kernel32.dll", EntryPoint = "GetDiskFreeSpaceExW", CharSet = CharSet.Unicode, SetLastError = true)]
    [DefaultDllImportSearchPaths(DllImportSearchPath.System32)]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static extern bool GetDiskFreeSpace(string directory, out ulong available, out ulong total, out ulong free);

    /// <summary>Gets the current user's available bytes on the nearest existing output directory.</summary>
    /// <param name="directory">The intended recording directory, which may not have been created yet.</param>
    public static ulong AvailableBytes(string directory)
    {
        var current = new DirectoryInfo(Path.GetFullPath(directory));
        while (!current.Exists && current.Parent is { } parent) current = parent;
        string path = Path.TrimEndingDirectorySeparator(current.FullName) + Path.DirectorySeparatorChar;
        if (!GetDiskFreeSpace(path, out ulong available, out _, out _))
            throw new Win32Exception(Marshal.GetLastWin32Error());
        return available;
    }

    /// <summary>Rejects a start below the configured boundary with an actionable English explanation.</summary>
    public static RecordingStorageStatus EnsureCanStart(string directory, Func<string, ulong>? query = null)
    {
        var status = new RecordingStorageStatus((query ?? AvailableBytes)(directory));
        if (status.MustStop)
            throw new InvalidOperationException($"Only {status.FreeSpace} of free disk space remain in the recording location. At least 50 MiB are required to start. Free disk space and try again.");
        return status;
    }
}

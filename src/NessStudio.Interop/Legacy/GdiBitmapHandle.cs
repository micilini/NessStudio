using Microsoft.Win32.SafeHandles;
using System.Runtime.InteropServices;

namespace NessStudio.Interop.Legacy;

/// <summary>Owns a bitmap returned by GetHbitmap and releases it through GDI.</summary>
public sealed partial class GdiBitmapHandle : SafeHandleZeroOrMinusOneIsInvalid
{
    /// <summary>Takes ownership of the supplied bitmap handle.</summary>
    /// <param name="bitmap">The owned bitmap handle, never borrowed from a control.</param>
    public GdiBitmapHandle(nint bitmap) : base(ownsHandle: true) => SetHandle(bitmap);
    /// <inheritdoc />
    protected override bool ReleaseHandle() => DeleteObject(handle) != 0;
    [LibraryImport("gdi32.dll", EntryPoint = "DeleteObject")]
    private static partial int DeleteObject(nint bitmap);
}

/// <summary>Reads actual CPU time for the calling worker without allocating a thread handle.</summary>
public static partial class ThreadCpuTime
{
    /// <summary>Gets kernel plus user time in 100 ns units, or null if Windows cannot supply it.</summary>
    /// <remarks>The pseudo handle is borrowed and must not be closed.</remarks>
    public static long? ReadCurrentThreadHns() => GetThreadTimes(GetCurrentThread(), out _, out _, out long kernel, out long user) != 0
        ? checked(kernel + user) : null;
    [LibraryImport("kernel32.dll", EntryPoint = "GetCurrentThread")]
    private static partial nint GetCurrentThread();
    [LibraryImport("kernel32.dll", EntryPoint = "GetThreadTimes")]
    private static partial int GetThreadTimes(nint thread, out long created, out long exited, out long kernel, out long user);
}

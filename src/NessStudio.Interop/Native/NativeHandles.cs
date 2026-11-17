using Microsoft.Win32.SafeHandles;

namespace NessStudio.Interop.Native;

/// <summary>Owns an engine context; creating the context does not open media resources.</summary>
internal sealed class EngineHandle : SafeHandleZeroOrMinusOneIsInvalid
{
    public EngineHandle() : base(true) { }
    protected override bool ReleaseHandle() { NativeMethods.EngineRelease(handle); return true; }
}

/// <summary>Owns one immutable blob independently of its originating context.</summary>
internal sealed class BlobHandle : SafeHandleZeroOrMinusOneIsInvalid
{
    public BlobHandle() : base(true) { }
    protected override bool ReleaseHandle() { NativeMethods.BlobRelease(handle); return true; }
}

/// <summary>Transfers an abandoned session to native asynchronous cleanup without blocking a finalizer or UI thread.</summary>
internal sealed class SessionHandle : SafeHandleZeroOrMinusOneIsInvalid
{
    public SessionHandle() : base(true) { }
    protected override bool ReleaseHandle() { NativeMethods.SessionRelease(handle); return true; }
}

/// <summary>Abandons a native job to asynchronous cancellation and cleanup.</summary>
internal sealed class JobHandle : SafeHandleZeroOrMinusOneIsInvalid
{
    public JobHandle() : base(true) { }
    protected override bool ReleaseHandle() { NativeMethods.JobRelease(handle); return true; }
}

/// <summary>Owns one detachable presentation consumer, independently of a session handle.</summary>
internal sealed class PreviewHandle : SafeHandleZeroOrMinusOneIsInvalid
{
    public PreviewHandle() : base(true) { }
    protected override bool ReleaseHandle() { NativeMethods.PreviewRelease(handle); return true; }
}

/// <summary>Retains one immutable presentation slot until the UI has finished using its storage.</summary>
internal sealed class PreviewLeaseHandle : SafeHandleZeroOrMinusOneIsInvalid
{
    public PreviewLeaseHandle() : base(true) { }
    protected override bool ReleaseHandle() { NativeMethods.PreviewLeaseRelease(handle); return true; }
}

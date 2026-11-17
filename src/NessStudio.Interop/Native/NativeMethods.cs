using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;

namespace NessStudio.Interop.Native;

internal static partial class NativeMethods
{
    private const string Library = "libnessstudio";
    [LibraryImport(Library, EntryPoint = "nss_preview_attach")]
    [UnmanagedCallConv(CallConvs = [typeof(CallConvCdecl)])]
    internal static partial int PreviewAttach(SessionHandle session, ref nss_preview_config_v1 config, out PreviewHandle preview, ref nss_error_v1 error);
    [LibraryImport(Library, EntryPoint = "nss_preview_set_enabled")]
    [UnmanagedCallConv(CallConvs = [typeof(CallConvCdecl)])]
    internal static partial int PreviewSetEnabled(PreviewHandle preview, uint enabled, ref nss_error_v1 error);
    [LibraryImport(Library, EntryPoint = "nss_preview_acquire")]
    [UnmanagedCallConv(CallConvs = [typeof(CallConvCdecl)])]
    internal static partial int PreviewAcquire(PreviewHandle preview, out PreviewLeaseHandle lease, ref nss_preview_frame_v1 frame, ref nss_error_v1 error);
    [LibraryImport(Library, EntryPoint = "nss_gpu_preview_acquire")]
    [UnmanagedCallConv(CallConvs = [typeof(CallConvCdecl)])]
    internal static partial int GpuPreviewAcquire(PreviewHandle preview, out PreviewLeaseHandle lease, ref nss_gpu_surface_v1 frame, ref nss_error_v1 error);
    [LibraryImport(Library, EntryPoint = "nss_preview_detach")]
    [UnmanagedCallConv(CallConvs = [typeof(CallConvCdecl)])]
    internal static partial int PreviewDetach(PreviewHandle preview, ref nss_error_v1 error);
    [LibraryImport(Library, EntryPoint = "nss_preview_release")]
    [UnmanagedCallConv(CallConvs = [typeof(CallConvCdecl)])]
    internal static partial void PreviewRelease(nint preview);
    [LibraryImport(Library, EntryPoint = "nss_preview_lease_release")]
    [UnmanagedCallConv(CallConvs = [typeof(CallConvCdecl)])]
    internal static partial void PreviewLeaseRelease(nint lease);
    [LibraryImport(Library, EntryPoint = "nss_engine_create")]
    [UnmanagedCallConv(CallConvs = [typeof(CallConvCdecl)])]
    internal static partial int EngineCreate(ref nss_json_v1 config, out EngineHandle engine, ref nss_error_v1 error);
    [LibraryImport(Library, EntryPoint = "nss_engine_release")]
    [UnmanagedCallConv(CallConvs = [typeof(CallConvCdecl)])]
    internal static partial void EngineRelease(nint engine);
    [LibraryImport(Library, EntryPoint = "nss_engine_capabilities")]
    [UnmanagedCallConv(CallConvs = [typeof(CallConvCdecl)])]
    internal static partial int Capabilities(EngineHandle engine, out BlobHandle blob, ref nss_error_v1 error);
    [LibraryImport(Library, EntryPoint = "nss_blob_view")]
    [UnmanagedCallConv(CallConvs = [typeof(CallConvCdecl)])]
    internal static partial int BlobView(BlobHandle blob, out nss_utf8_view view);
    [LibraryImport(Library, EntryPoint = "nss_blob_release")]
    [UnmanagedCallConv(CallConvs = [typeof(CallConvCdecl)])]
    internal static partial void BlobRelease(nint blob);
    [LibraryImport(Library, EntryPoint = "nss_runtime_quiesce")]
    [UnmanagedCallConv(CallConvs = [typeof(CallConvCdecl)])]
    internal static partial int Quiesce(uint timeout, ref nss_error_v1 error);
    [LibraryImport(Library, EntryPoint = "nss_session_create")]
    [UnmanagedCallConv(CallConvs = [typeof(CallConvCdecl)])]
    internal static partial int SessionCreate(EngineHandle engine, ref nss_json_v1 request, out SessionHandle session, ref nss_error_v1 error);
    [LibraryImport(Library, EntryPoint = "nss_session_command")]
    [UnmanagedCallConv(CallConvs = [typeof(CallConvCdecl)])]
    internal static partial int SessionCommand(SessionHandle session, uint command, out ulong id, ref nss_error_v1 error);
    [LibraryImport(Library, EntryPoint = "nss_session_snapshot")]
    [UnmanagedCallConv(CallConvs = [typeof(CallConvCdecl)])]
    internal static partial int SessionSnapshot(SessionHandle session, out BlobHandle snapshot, ref nss_error_v1 error);
    [LibraryImport(Library, EntryPoint = "nss_session_update_scene")]
    [UnmanagedCallConv(CallConvs = [typeof(CallConvCdecl)])]
    internal static partial int SessionUpdateScene(SessionHandle session, nss_utf8_view scene, out ulong revision, ref nss_error_v1 error);
    [LibraryImport(Library, EntryPoint = "nss_session_poll_event")]
    [UnmanagedCallConv(CallConvs = [typeof(CallConvCdecl)])]
    internal static partial int SessionPoll(SessionHandle session, uint timeout, ref nss_event_v1 value, ref nss_error_v1 error);
    [LibraryImport(Library, EntryPoint = "nss_session_interrupt_poll")]
    [UnmanagedCallConv(CallConvs = [typeof(CallConvCdecl)])]
    internal static partial void SessionInterrupt(SessionHandle session);
    [LibraryImport(Library, EntryPoint = "nss_session_release")]
    [UnmanagedCallConv(CallConvs = [typeof(CallConvCdecl)])]
    internal static partial void SessionRelease(nint session);
    [LibraryImport(Library, EntryPoint = "nss_probe_software_encoder")]
    [UnmanagedCallConv(CallConvs = [typeof(CallConvCdecl)])]
    internal static partial int ProbeEncoder(EngineHandle engine, nss_utf8_view path, out BlobHandle result, ref nss_error_v1 error);
    [LibraryImport(Library, EntryPoint = "nss_engine_devices")]
    [UnmanagedCallConv(CallConvs = [typeof(CallConvCdecl)])]
    internal static partial int Devices(EngineHandle engine, uint kind, out BlobHandle result, ref nss_error_v1 error);
    [LibraryImport(Library, EntryPoint = "nss_probe_capture")]
    [UnmanagedCallConv(CallConvs = [typeof(CallConvCdecl)])]
    internal static partial int ProbeCapture(EngineHandle engine, ref nss_json_v1 request, out BlobHandle result, ref nss_error_v1 error);
    [LibraryImport(Library, EntryPoint = "nss_job_create")]
    [UnmanagedCallConv(CallConvs = [typeof(CallConvCdecl)])]
    internal static partial int JobCreate(EngineHandle engine, ref nss_json_v1 request, out JobHandle job, ref nss_error_v1 error);
    [LibraryImport(Library, EntryPoint = "nss_job_start")]
    [UnmanagedCallConv(CallConvs = [typeof(CallConvCdecl)])]
    internal static partial int JobStart(JobHandle job, out ulong command, ref nss_error_v1 error);
    [LibraryImport(Library, EntryPoint = "nss_job_cancel")]
    [UnmanagedCallConv(CallConvs = [typeof(CallConvCdecl)])]
    internal static partial int JobCancel(JobHandle job, out ulong command, ref nss_error_v1 error);
    [LibraryImport(Library, EntryPoint = "nss_job_snapshot")]
    [UnmanagedCallConv(CallConvs = [typeof(CallConvCdecl)])]
    internal static partial int JobSnapshot(JobHandle job, out BlobHandle snapshot, ref nss_error_v1 error);
    [LibraryImport(Library, EntryPoint = "nss_job_poll_event")]
    [UnmanagedCallConv(CallConvs = [typeof(CallConvCdecl)])]
    internal static partial int JobPoll(JobHandle job, uint timeout, ref nss_event_v1 value, ref nss_error_v1 error);
    [LibraryImport(Library, EntryPoint = "nss_job_interrupt_poll")]
    [UnmanagedCallConv(CallConvs = [typeof(CallConvCdecl)])]
    internal static partial void JobInterrupt(JobHandle job);
    [LibraryImport(Library, EntryPoint = "nss_job_release")]
    [UnmanagedCallConv(CallConvs = [typeof(CallConvCdecl)])]
    internal static partial void JobRelease(nint job);
}

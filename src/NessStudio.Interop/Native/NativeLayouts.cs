using System.Runtime.InteropServices;

namespace NessStudio.Interop.Native;

// ABI declarations: MSVC x64 default alignment, fixed integers and bounded UTF-8.
[StructLayout(LayoutKind.Sequential)]
internal struct nss_utf8_view { public nint data; public ulong bytes; }

[StructLayout(LayoutKind.Sequential)]
internal unsafe struct nss_version_v1
{
    public uint size, version, abi_major, abi_minor, pointer_bits, contract_revision;
    public fixed byte build_utf8[64];
}

[StructLayout(LayoutKind.Sequential)]
internal unsafe struct nss_error_v1
{
    public uint size, version;
    public int code;
    public uint stage;
    public int hresult;
    public uint reserved;
    public fixed byte message_utf8[256];
}

[StructLayout(LayoutKind.Sequential)]
internal struct nss_json_v1 { public uint size, version; public nss_utf8_view json; }

[StructLayout(LayoutKind.Sequential)]
internal unsafe struct nss_event_v1
{
    public uint size, version;
    public ulong sequence, command_id, object_id;
    public long active_time_hns;
    public uint kind, state;
    public int status;
    public uint stage;
    public int hresult;
    public uint reserved;
    public fixed byte message_utf8[256];
}

[StructLayout(LayoutKind.Sequential)]
internal struct nss_preview_config_v1
{
    public uint size, version;
    public ulong source_id;
    public uint max_width, max_height, fps_num, fps_den, slots, channel, reserved;
}

[StructLayout(LayoutKind.Sequential)]
internal struct nss_preview_frame_v1
{
    public uint size, version;
    public ulong session_id, source_id, generation, frame_id;
    public long capture_time_hns;
    public ulong acquisition_uncertainty_hns, bytes;
    public nint data;
    public uint width, height, stride, pixel_format, flags, reserved;
}

[StructLayout(LayoutKind.Sequential)]
internal struct nss_gpu_surface_v1
{
    public uint size, version;
    public ulong session_id, source_id, generation, frame_id;
    public long capture_time_hns;
    public nint surface_d3d9;
    public ulong adapter_luid;
    public uint width, height, flags, reserved;
}

[StructLayout(LayoutKind.Sequential)]
internal struct nss_packet_info_v1
{
    public uint size, version, track_id, flags;
    public long pts_hns, dts_hns, duration_hns;
    public ulong bytes;
}

using System.Reflection;
using System.Runtime.InteropServices;
using System.Text;

namespace NessStudio.Interop.Native;

internal static class NativeLibraryLoader
{
    private static readonly object Sync = new();
    private static nint _module;
    private static string? _path;
    private static NativeBuildInformation? _build;
    internal static readonly UTF8Encoding Utf8 = new(false, true);
    internal static bool IsLoaded => Volatile.Read(ref _module) != 0;

    internal static unsafe NativeBuildInformation Load(string directory)
    {
        ArgumentException.ThrowIfNullOrWhiteSpace(directory);
        string path = Path.Combine(Path.GetFullPath(directory), "libnessstudio.dll");
        lock (Sync)
        {
            if (_module != 0)
            {
                if (!string.Equals(path, _path, StringComparison.OrdinalIgnoreCase))
                    throw new InvalidOperationException("One native runtime path is supported per process; use a separate process to inspect another package.");
                return _build!;
            }
            if (!File.Exists(path)) throw new FileNotFoundException("libnessstudio.dll was not found in the controlled application directory. Build/publish the native targets.", path);
            using (var reader = new BinaryReader(File.OpenRead(path)))
            {
                if (reader.BaseStream.Length < 64 || reader.ReadUInt16() != 0x5a4d) throw new BadImageFormatException("Invalid native PE image", path);
                reader.BaseStream.Position = 0x3c;
                int offset = reader.ReadInt32();
                if (offset < 64 || offset > reader.BaseStream.Length - 6) throw new BadImageFormatException("Invalid native PE header", path);
                reader.BaseStream.Position = offset;
                if (reader.ReadUInt32() != 0x4550 || reader.ReadUInt16() != 0x8664 || !Environment.Is64BitProcess)
                    throw new BadImageFormatException("Native architecture mismatch: Windows x64 is required", path);
            }
            nint module = 0;
            try
            {
                module = NativeLibrary.Load(path, typeof(NativeMethods).Assembly,
                    DllImportSearchPath.UseDllDirectoryForDependencies | DllImportSearchPath.System32);
                var getVersion = (delegate* unmanaged[Cdecl]<nss_version_v1*, int>)NativeLibrary.GetExport(module, "nss_get_version");
                nss_version_v1 version = new() { size = (uint)sizeof(nss_version_v1), version = 1 };
                if (getVersion(&version) != 0 || version.abi_major != 1 || version.pointer_bits != 64 || version.contract_revision != 1)
                    throw new BadImageFormatException("Native ABI or contract revision is incompatible with this wrapper", path);
                foreach (string export in new[] { "nss_engine_create", "nss_engine_release", "nss_engine_capabilities", "nss_blob_view", "nss_blob_release", "nss_runtime_quiesce", "nss_session_create", "nss_probe_software_encoder" })
                    _ = NativeLibrary.GetExport(module, export);
                if (version.abi_minor >= 5)
                    foreach (string export in new[] { "nss_engine_devices", "nss_preview_attach", "nss_preview_acquire", "nss_gpu_preview_acquire",
                        "nss_preview_detach", "nss_preview_lease_release", "nss_preview_release" })
                        _ = NativeLibrary.GetExport(module, export);
                int buildLength = new ReadOnlySpan<byte>(version.build_utf8, 64).IndexOf((byte)0);
                if (buildLength <= 0) throw new BadImageFormatException("Native build identity is missing or not terminated", path);
                string build = Utf8.GetString(new ReadOnlySpan<byte>(version.build_utf8, buildLength));
                NativeLibrary.SetDllImportResolver(typeof(NativeMethods).Assembly, Resolve);
                _path = path;
                _build = new(version.abi_major, version.abi_minor, version.pointer_bits, version.contract_revision, build, path);
                Volatile.Write(ref _module, module); // Retained until process exit; SafeHandle finalizers may still call exports.
                return _build;
            }
            catch
            {
                if (module != 0) NativeLibrary.Free(module);
                throw;
            }
        }
    }

    private static nint Resolve(string name, Assembly assembly, DllImportSearchPath? searchPath)
        => name == "libnessstudio" ? Volatile.Read(ref _module) : 0;
}

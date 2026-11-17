using System.Runtime.InteropServices;
using System.Text.Json;

namespace NessStudio.Interop.Native;

/// <summary>Identifies the native DLL validated before any context is created.</summary>
/// <param name="AbiMajor">The ABI major.</param><param name="AbiMinor">The ABI minor.</param>
/// <param name="PointerBits">The native pointer width.</param><param name="ContractRevision">The contract revision.</param>
/// <param name="Build">The source/configuration identity.</param><param name="LibraryPath">The controlled absolute library path.</param>
public sealed record NativeBuildInformation(uint AbiMajor, uint AbiMinor, uint PointerBits, uint ContractRevision, string Build, string LibraryPath);

/// <summary>Preserves the native result, stage and original HRESULT without consulting GetLastError.</summary>
public sealed class NativeEngineException : Exception
{
    /// <summary>Gets the stable native result code.</summary>
    public int ResultCode { get; }
    /// <summary>Gets the stage responsible for the failure.</summary>
    public uint Stage { get; }
    /// <summary>Gets the original native HRESULT, or zero when unavailable.</summary>
    public int OriginalHResult { get; }
    /// <summary>Preserves a failed native command or snapshot returned through the same versioned ABI.</summary>
    public NativeEngineException(int result, uint stage, int hresult, string message) : base(message)
    { ResultCode = result; Stage = stage; OriginalHResult = hresult; if (hresult != 0) HResult = hresult; }
}

/// <summary>Owns a real native bootstrap context, independent of WPF and the legacy recording factory.</summary>
/// <remarks>Sessions and export jobs own native workers independently. Owners await their asynchronous shutdown before disposing the context.</remarks>
public sealed class NativeEngine : IDisposable
{
    private readonly EngineHandle _handle;
    /// <summary>Gets the native ABI and build information verified before creation.</summary>
    public NativeBuildInformation Build { get; }
    private NativeEngine(EngineHandle handle, NativeBuildInformation build) { _handle = handle; Build = build; }

    /// <summary>Loads the application-owned DLL and copies a versioned Unicode configuration into a real native context.</summary>
    /// <param name="applicationDirectory">The controlled package directory; PATH is never searched for the engine.</param>
    /// <param name="clientName">The optional diagnostic client identity, at most 1024 UTF-8 bytes without NUL.</param>
    /// <returns>The owned context.</returns>
    public static unsafe NativeEngine Open(string applicationDirectory, string clientName = "NessStudio")
    {
        ArgumentNullException.ThrowIfNull(clientName);
        var build = NativeLibraryLoader.Load(applicationDirectory);
        byte[] config = NativeLibraryLoader.Utf8.GetBytes(JsonSerializer.Serialize(new { version = 1, clientName }));
        fixed (byte* data = config)
        {
            nss_json_v1 request = new() { size = (uint)sizeof(nss_json_v1), version = 1, json = new() { data = (nint)data, bytes = (ulong)config.Length } };
            var error = NewError();
            int code = NativeMethods.EngineCreate(ref request, out var handle, ref error);
            if (code != 0) { handle.Dispose(); Throw(code, error); }
            return new(handle, build);
        }
    }

    /// <summary>Gets a copied JSON snapshot of compiled capabilities without opening devices or inventing recording support.</summary>
    /// <returns>A caller-owned document.</returns>
    public JsonDocument GetCapabilities()
    {
        var error = NewError();
        int code = NativeMethods.Capabilities(_handle, out var blob, ref error);
        using (blob) { if (code != 0) Throw(code, error); return Copy(blob); }
    }

    /// <summary>Enumerates physical monitor identities and geometry without creating a capture or graphics device.</summary>
    /// <returns>A caller-owned JSON document with stable monitor IDs and physical pixel bounds.</returns>
    public JsonDocument GetScreenSources()
    {
        var error = NewError();
        int result = NativeMethods.Devices(_handle, 1, out var blob, ref error);
        using (blob) { if (result != 0) Throw(result, error); return Copy(blob); }
    }

    /// <summary>Enumerates stable IDs and actual shared formats for active audio endpoints.</summary>
    /// <param name="loopback">Selects render loopback endpoints; otherwise selects microphones.</param>
    /// <returns>A caller-owned device document.</returns>
    public JsonDocument GetAudioSources(bool loopback)
    {
        var error = NewError();
        int result = NativeMethods.Devices(_handle, loopback ? 4u : 3u, out var blob, ref error);
        using (blob) { if (result != 0) Throw(result, error); return Copy(blob); }
    }

    /// <summary>Enumerates stable native camera identities and explicitly supported input formats.</summary>
    public JsonDocument GetCameraSources()
    {
        if (Build.AbiMinor < 5) throw new NotSupportedException("Native cameras require ABI 1.5.");
        var error = NewError();
        int result = NativeMethods.Devices(_handle, 2, out var blob, ref error);
        using (blob) { if (result != 0) Throw(result, error); return Copy(blob); }
    }

    /// <summary>Creates a native av-basic FFmpeg job from complete session assets.</summary>
    /// <param name="request">Versioned export description containing explicit executable and asset paths.</param>
    /// <returns>The owned asynchronous job.</returns>
    public unsafe NativeJob CreateJob(JsonElement request)
    {
        if (Build.AbiMinor < 3) throw new NotSupportedException("Audio/export jobs require native ABI 1.3.");
        byte[] bytes = NativeLibraryLoader.Utf8.GetBytes(request.GetRawText());
        fixed (byte* data = bytes)
        {
            nss_json_v1 control = new() { size = (uint)sizeof(nss_json_v1), version = 1, json = new() { data = (nint)data, bytes = (ulong)bytes.Length } };
            var error = NewError();
            int result = NativeMethods.JobCreate(_handle, ref control, out var handle, ref error);
            if (result != 0) { handle.Dispose(); Throw(result, error); }
            return new NativeJob(handle);
        }
    }

    /// <summary>Runs a bounded synchronous native capture and processing probe on its own worker.</summary>
    /// <param name="request">Versioned JSON selecting an explicit route, source, geometry, cadence and duration.</param>
    /// <returns>A caller-owned metrics document. This probe creates no encoded recording.</returns>
    /// <remarks>Requires ABI 1.2. Call from a background operation when a UI must remain responsive.</remarks>
    public unsafe JsonDocument ProbeCapture(JsonElement request)
    {
        if (Build.AbiMinor < 2) throw new NotSupportedException("Capture probes require native ABI 1.2 or newer.");
        byte[] bytes = NativeLibraryLoader.Utf8.GetBytes(request.GetRawText());
        fixed (byte* data = bytes)
        {
            nss_json_v1 control = new() { size = (uint)sizeof(nss_json_v1), version = 1, json = new() { data = (nint)data, bytes = (ulong)bytes.Length } };
            var error = NewError();
            int result = NativeMethods.ProbeCapture(_handle, ref control, out var blob, ref error);
            using (blob) { if (result != 0) Throw(result, error); return Copy(blob); }
        }
    }

    /// <summary>Creates an owned session from a versioned plan; unimplemented physical backends fail explicitly.</summary>
    /// <param name="request">The caller-owned control JSON, copied before returning.</param>
    /// <returns>The session whose commands complete only after correlated native events.</returns>
    /// <remarks>Synthetic requests are supported exclusively by the isolated validation DLL, never the shipped engine.</remarks>
    public unsafe NativeSession CreateSession(JsonElement request)
    {
        if (Build.AbiMinor < 1) throw new NotSupportedException("Session infrastructure requires native ABI 1.1 or newer.");
        byte[] bytes = NativeLibraryLoader.Utf8.GetBytes(request.GetRawText());
        fixed (byte* data = bytes)
        {
            nss_json_v1 control = new() { size = (uint)sizeof(nss_json_v1), version = 1, json = new() { data = (nint)data, bytes = (ulong)bytes.Length } };
            var error = NewError();
            int result = NativeMethods.SessionCreate(_handle, ref control, out var handle, ref error);
            if (result != 0) { handle.Dispose(); Throw(result, error); }
            try { return new NativeSession(handle); } catch { handle.Dispose(); throw; }
        }
    }

    /// <summary>Runs a synchronous, explicit software H.264 diagnostic using generated CPU pixels, never a capture source.</summary>
    /// <param name="newAbsolutePath">The new fixture path. The current legacy writer diagnostic requires ASCII paths.</param>
    /// <returns>A caller-owned report; independent media decoding is still required to qualify the resulting fixture.</returns>
    public unsafe JsonDocument ProbeSoftwareEncoder(string newAbsolutePath)
    {
        byte[] path = NativeLibraryLoader.Utf8.GetBytes(newAbsolutePath);
        fixed (byte* data = path)
        {
            var error = NewError();
            int code = NativeMethods.ProbeEncoder(_handle, new() { data = (nint)data, bytes = (ulong)path.Length }, out var blob, ref error);
            using (blob) { if (code != 0) Throw(code, error); return Copy(blob); }
        }
    }

    /// <summary>Checks whether native contexts, sessions, blobs, workers, frame leases and pools have quiesced.</summary>
    /// <param name="timeoutMilliseconds">The bounded wait, from zero to 1000 milliseconds.</param>
    /// <returns>True on quiescence; false on timeout.</returns>
    public static bool WaitForQuiescence(uint timeoutMilliseconds = 0)
    {
        if (!NativeLibraryLoader.IsLoaded) throw new InvalidOperationException("Open a controlled native runtime before querying quiescence.");
        var error = NewError();
        int result = NativeMethods.Quiesce(timeoutMilliseconds, ref error);
        if (result == 3) return false;
        if (result != 0) Throw(result, error);
        return true;
    }
    internal static unsafe nss_error_v1 NewError() => new() { size = (uint)sizeof(nss_error_v1), version = 1 };
    internal static unsafe void Throw(int code, nss_error_v1 error)
    {
        byte* bytes = error.message_utf8;
        int length = new ReadOnlySpan<byte>(bytes, 256).IndexOf((byte)0);
        string message = NativeLibraryLoader.Utf8.GetString(new ReadOnlySpan<byte>(bytes, length < 0 ? 256 : length));
        throw new NativeEngineException(code, error.stage, error.hresult, message);
    }
    internal static unsafe JsonDocument Copy(BlobHandle blob)
    {
        int code = NativeMethods.BlobView(blob, out var view);
        if (code != 0 || view.data == 0 || view.bytes > 1048576) throw new InvalidDataException("Invalid native blob view.");
        string value = NativeLibraryLoader.Utf8.GetString(new ReadOnlySpan<byte>((void*)view.data, checked((int)view.bytes)));
        GC.KeepAlive(blob);
        return JsonDocument.Parse(value);
    }
    /// <summary>Releases this basic native context exactly once.</summary>
    public void Dispose() => _handle.Dispose();
}

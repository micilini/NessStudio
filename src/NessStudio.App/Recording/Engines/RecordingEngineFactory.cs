using NessStudio.Diagnostics;

namespace NessStudio.Recording.Engines;

/// <summary>Routes all supported recording presets through the native engine.</summary>
public sealed class RecordingEngineFactory(string applicationDirectory, IDiagnosticLog? diagnosticLog = null) : IRecorderEngineFactory
{
    private readonly IRecorderEngineFactory _native = new NativeRecorderEngineFactory(applicationDirectory, diagnosticLog);
    /// <inheritdoc />
    public RecordingCapabilities Capabilities => _native.Capabilities;
    /// <inheritdoc />
    public IRecordingService Create(RecordingRequest request)
    {
        return _native.Create(request);
    }
}

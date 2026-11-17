namespace NessStudio.Diagnostics;

/// <summary>Accepts local diagnostic messages without exposing a frontend or file writer.</summary>
public interface IDiagnosticLog
{
    /// <summary>Writes a diagnostic message when permitted by the configured policy.</summary>
    /// <param name="message">The diagnostic message, excluding credentials and private media.</param>
    void Write(string message);
}

/// <summary>Discards optional diagnostics when no sink was supplied.</summary>
public sealed class NullDiagnosticLog : IDiagnosticLog
{
    private NullDiagnosticLog() { }
    /// <summary>Gets the shared no-output diagnostic sink.</summary>
    public static NullDiagnosticLog Instance { get; } = new();
    /// <inheritdoc />
    public void Write(string message) { }
}

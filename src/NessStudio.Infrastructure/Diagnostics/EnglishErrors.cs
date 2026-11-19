using System.ComponentModel;
using System.Runtime.InteropServices;
using System.Text;

namespace NessStudio.Diagnostics;

/// <summary>Formats application errors in English even when Windows system messages use another language.</summary>
public static class EnglishErrors
{
    [DllImport("kernel32.dll", EntryPoint = "FormatMessageW", CharSet = CharSet.Unicode)]
    [DefaultDllImportSearchPaths(DllImportSearchPath.System32)]
    private static extern uint FormatMessage(uint flags, nint source, uint code, uint language,
        StringBuilder buffer, uint capacity, nint arguments);

    /// <summary>Returns an English description while retaining file names and native error codes.</summary>
    public static string Message(Exception? error)
    {
        if (error is null) return string.Empty;
        if (error is AggregateException aggregate)
            return string.Join(" ", aggregate.Flatten().InnerExceptions.Select(Message));
        if (error is ExternalException external && error is not Win32Exception &&
            (unchecked((uint)external.ErrorCode) & 0xffff0000u) != 0x80070000u)
        {
            string description = unchecked((uint)external.ErrorCode) switch
            {
                0x80004001 => "The operation is not implemented by this component.",
                0x80004002 => "The requested component interface is not supported.",
                0x80004003 => "The component received an invalid pointer.",
                0x80004004 => "The component aborted the operation.",
                0x80004005 => "An external component reported an unspecified error.",
                0x80010108 => "The external component disconnected.",
                0x8001010e => "The component was called from the wrong thread.",
                0x80040154 => "A required component is not registered.",
                0x80080005 => "The external component could not start.",
                _ => "An external component reported an error."
            };
            return description + $" (HRESULT 0x{unchecked((uint)external.ErrorCode):X8})";
        }
        int? code = error is Win32Exception native ? native.NativeErrorCode :
            (error is IOException or UnauthorizedAccessException or ExternalException ||
             error is NessStudio.Interop.Native.NativeEngineException && (error.HResult & 0xffff) is 39 or 112) &&
            (unchecked((uint)error.HResult) & 0xffff0000u) == 0x80070000u ? error.HResult & 0xffff : null;
        if (code is not { } value) return error.Message;
        // Some localized Windows installations do not contain English system
        // message resources. Keep common recording/storage errors useful there.
        string message = value switch
        {
            2 => "The specified file was not found.",
            3 => "The specified path was not found.",
            5 => "Access is denied.",
            6 => "The handle is invalid.",
            8 or 14 => "There is not enough memory to complete the operation.",
            19 => "The storage device is write-protected.",
            21 => "The device is not ready.",
            32 => "The file is being used by another process.",
            33 => "Another process has locked part of the file.",
            38 => "The end of the file was reached.",
            39 or 112 => "There is not enough free disk space.",
            50 => "The operation is not supported.",
            53 => "The network path was not found.",
            64 => "The network connection was lost.",
            80 or 183 => "The file already exists.",
            87 => "A parameter is invalid.",
            121 or 258 => "The operation timed out.",
            123 => "The file name or directory path is invalid.",
            126 => "A required module could not be found.",
            127 => "A required procedure could not be found.",
            206 => "The file name or path is too long.",
            995 or 1223 => "The operation was canceled.",
            996 or 997 => "The input/output operation has not completed yet.",
            1117 => "The storage device reported an input/output error.",
            1167 => "The device is not connected.",
            _ => string.Empty
        };
        if (message.Length == 0)
        {
            var text = new StringBuilder(2048);
            uint length = FormatMessage(0x1200, 0, unchecked((uint)value), 0x0409, text, (uint)text.Capacity, 0);
            message = length == 0 ? "An operating system error occurred." : text.ToString().Trim();
        }
        string? file = error switch { FileNotFoundException missing => missing.FileName,
            FileLoadException load => load.FileName, _ => null };
        return message + $" (Windows error {value})" + (string.IsNullOrEmpty(file) ? string.Empty : " File: " + file);
    }

    /// <summary>Preserves the exception chain and stack locations with English system descriptions.</summary>
    public static string Details(Exception? error)
    {
        if (error is null) return "An unexpected application error occurred.";
        return error.GetType().FullName + ": " + Message(error) + Environment.NewLine + error.StackTrace +
            (error.InnerException is null ? string.Empty : Environment.NewLine + "Caused by: " + Details(error.InnerException));
    }
}

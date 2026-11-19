using System.ComponentModel;
using System.Runtime.InteropServices;
using NessStudio.Diagnostics;

internal static partial class Program
{
    private static void EnglishErrorCases()
    {
        Case("english-access-denied-independent-of-windows-language", () =>
            Check(EnglishErrors.Message(new Win32Exception(5, "Localized native message")) ==
                "Access is denied. (Windows error 5)", "Localized Windows error escaped into the application."));
        Case("english-disk-full-error", () =>
            Check(EnglishErrors.Message(new Win32Exception(112)).Contains("free disk space"), "Disk-full explanation was lost."));
        Case("english-system-error-preserves-file-name", () =>
        {
            const string file = "C:/recordings/recording.mkv";
            Check(EnglishErrors.Message(new FileNotFoundException("Localized file message", file)).Contains(file),
                "English file error discarded the affected file name.");
        });
        Case("english-aggregate-native-errors", () =>
        {
            string text = EnglishErrors.Message(new AggregateException(new Win32Exception(5, "Localized A"),
                new IOException("Localized B", unchecked((int)0x80070020))));
            Check(text.Contains("Access is denied") && text.Contains("another process") && !text.Contains("Localized"),
                "An aggregate retained localized Windows descriptions.");
        });
        Case("english-unknown-native-error-retains-code", () =>
            Check(EnglishErrors.Message(new Win32Exception(999999)).Contains("Windows error 999999"),
                "An unknown error lost its diagnostic code."));
        Case("english-com-error-independent-of-native-message", () =>
        {
            var text = EnglishErrors.Message(new COMException("Localized COM failure", unchecked((int)0x80004005)));
            Check(text.Contains("unspecified error") && text.Contains("80004005") && !text.Contains("Localized"),
                "A COM component retained its localized system description.");
        });
        Case("english-unknown-com-error-retains-hresult", () =>
        {
            var text = EnglishErrors.Message(new COMException("Localized component failure", unchecked((int)0x8234ABCD)));
            Check(text.Contains("external component") && text.Contains("8234ABCD") && !text.Contains("Localized"),
                "An unknown component error lost its HRESULT or retained localized text.");
        });
    }
}

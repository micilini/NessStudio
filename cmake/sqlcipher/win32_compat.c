/* Preserve the old native provider's exported OS-family query. Upstream
 * SQLite removed this historical Win9x distinction. Every OS supported by
 * this Windows x64 runtime belongs to the NT family; this is not a codec,
 * database, hardware-capability, or operating-system-version probe. */
#if !defined(_WIN64)
#error The compatibility export requires the Windows x64 runtime contract.
#endif
__declspec(dllexport) int sqlite3_win32_is_nt(void) { return 1; }

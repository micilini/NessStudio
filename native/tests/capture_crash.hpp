#pragma once
#include <dbghelp.h>
#include <cstdio>
// Test-process diagnostics only. No desktop/user memory is written to disk.
inline LONG WINAPI capture_exception(EXCEPTION_POINTERS *exception) {
    const auto process = GetCurrentProcess(), thread = GetCurrentThread();
    std::fprintf(stderr, "capture-test exception=%08lx address=%p\n",
                 exception->ExceptionRecord->ExceptionCode, exception->ExceptionRecord->ExceptionAddress);
    SymSetOptions(SYMOPT_LOAD_LINES | SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS);
    if (SymInitialize(process, nullptr, TRUE)) {
        CONTEXT context = *exception->ContextRecord;
        STACKFRAME64 frame{};
        frame.AddrPC = {context.Rip, 0, AddrModeFlat};
        frame.AddrStack = {context.Rsp, 0, AddrModeFlat};
        frame.AddrFrame = {context.Rbp, 0, AddrModeFlat};
        for (int i = 0; i < 24; ++i) {
            char storage[sizeof(SYMBOL_INFO) + 1024]{};
            auto *symbol = reinterpret_cast<SYMBOL_INFO *>(storage);
            symbol->SizeOfStruct = sizeof(SYMBOL_INFO);
            symbol->MaxNameLen = 1023;
            DWORD64 displacement{};
            if (SymFromAddr(process, frame.AddrPC.Offset, &displacement, symbol)) {
                IMAGEHLP_LINE64 line{};
                line.SizeOfStruct = sizeof(line);
                DWORD offset{};
                const bool has_line =
                    SymGetLineFromAddr64(process, frame.AddrPC.Offset, &offset, &line) != FALSE;
                std::fprintf(stderr, "%s +%llu %s:%lu\n", symbol->Name,
                             static_cast<unsigned long long>(displacement), has_line ? line.FileName : "",
                             has_line ? line.LineNumber : 0);
            } else
                std::fprintf(stderr, "address=%llx\n", static_cast<unsigned long long>(frame.AddrPC.Offset));
            if (!StackWalk64(IMAGE_FILE_MACHINE_AMD64, process, thread, &frame, &context, nullptr,
                             SymFunctionTableAccess64, SymGetModuleBase64, nullptr))
                break;
        }
        SymCleanup(process);
    }
    return EXCEPTION_EXECUTE_HANDLER;
}

#pragma once
// Crash-recorder stack walk (x64): return addresses from an exception CONTEXT, each as module+offset, so a crash log
// names the module under the fault — ours or the game's. No dbghelp: RtlLookupFunctionEntry/RtlVirtualUnwind from
// ntdll, and every module UE builds carries unwind data (.pdata). Read-only: walks a copy, never the live context.
#include <Windows.h>
#include <cstdio>
#include <cstring>
#include <string>

namespace unwind {

// "Archon-Win64-Shipping.exe+2994f72" for one address; "?" when no loaded module owns it.
inline std::string where(void* pc) {
    HMODULE mod = nullptr;
    char name[MAX_PATH] = "?";
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           static_cast<LPCSTR>(pc), &mod))
        GetModuleFileNameA(mod, name, MAX_PATH);
    const char* base = std::strrchr(name, '\\');
    char buf[MAX_PATH + 32];
    std::snprintf(buf, sizeof buf, "%s+%llx", base ? base + 1 : name,
                  (unsigned long long)((uintptr_t)pc - (uintptr_t)mod));
    return buf;
}

// Up to max return addresses, innermost first, from ctx (the fault's own context: [0] = the faulting instruction).
// Stops at the first frame without unwind data (leaf, jitted code) or any step that does not advance.
inline size_t walk(const CONTEXT* ctx, void** out, size_t max) {
    using LookupFn = PRUNTIME_FUNCTION(NTAPI*)(DWORD64, PDWORD64, PUNWIND_HISTORY_TABLE);
    using UnwindFn = PVOID(NTAPI*)(ULONG, DWORD64, DWORD64, PRUNTIME_FUNCTION, PCONTEXT, PVOID*, PDWORD64,
                                   PKNONVOLATILE_CONTEXT_POINTERS);
    HMODULE ntdll = GetModuleHandleA("ntdll.dll");
    static LookupFn lookup = ntdll ? reinterpret_cast<LookupFn>(GetProcAddress(ntdll, "RtlLookupFunctionEntry")) : nullptr;
    static UnwindFn unwindFn = ntdll ? reinterpret_cast<UnwindFn>(GetProcAddress(ntdll, "RtlVirtualUnwind")) : nullptr;
    if (!lookup || !unwindFn || !ctx || !ctx->Rip || !max) return 0;

    CONTEXT c = *ctx;
    size_t n = 0;
    out[n++] = reinterpret_cast<void*>(c.Rip);
    UNWIND_HISTORY_TABLE hist{};
    while (n < max) {
        DWORD64 imageBase = 0;
        PRUNTIME_FUNCTION fn = lookup(c.Rip, &imageBase, &hist);
        if (!fn) break;
        const DWORD64 pc = c.Rip, sp = c.Rsp;
        PVOID handlerData = nullptr;
        DWORD64 frame = 0;
        unwindFn(0 /* UNW_FLAG_NHANDLER */, imageBase, pc, fn, &c, &handlerData, &frame, nullptr);
        if (!c.Rip || (c.Rip == pc && c.Rsp <= sp)) break;  // unwound onto itself: stop, never loop
        out[n++] = reinterpret_cast<void*>(c.Rip);
    }
    return n;
}

}  // namespace unwind
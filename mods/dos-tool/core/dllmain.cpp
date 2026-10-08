#include <Windows.h>
#include <cstdio>
#include <cstring>
#include <string>

#include "logger.hpp"
#include "game.hpp"
#include "overlay.hpp"

// Crash recorder: fatal-looking exceptions (any module) go to dos-tool.log as module+offset, because some
// crashes leave no UE crash report and no Windows event. First-chance: a handled one is logged too, hence "maybe".
static PVOID g_veh = nullptr;
static volatile LONG g_vehLogged = 0;

static LONG CALLBACK CrashRecorder(EXCEPTION_POINTERS* ep) {
    const DWORD code = ep->ExceptionRecord->ExceptionCode;
    if (code != EXCEPTION_ACCESS_VIOLATION && code != EXCEPTION_STACK_OVERFLOW && code != EXCEPTION_ILLEGAL_INSTRUCTION
        && code != EXCEPTION_PRIV_INSTRUCTION && code != 0xC0000409 /* fast fail */)
        return EXCEPTION_CONTINUE_SEARCH;
    if (InterlockedIncrement(&g_vehLogged) > 8) return EXCEPTION_CONTINUE_SEARCH;
    void* at = ep->ExceptionRecord->ExceptionAddress;
    HMODULE mod = nullptr;
    char name[MAX_PATH] = "?";
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           static_cast<LPCSTR>(at), &mod))
        GetModuleFileNameA(mod, name, MAX_PATH);
    const char* base = strrchr(name, '\\');
    char buf[400];
    std::snprintf(buf, sizeof(buf), "[crash maybe] code=%08lx at %s+%llx thread=%lu addr=%llx", code, base ? base + 1 : name,
                  (unsigned long long)((uintptr_t)at - (uintptr_t)mod), GetCurrentThreadId(),
                  (unsigned long long)(ep->ExceptionRecord->NumberParameters > 1 ? ep->ExceptionRecord->ExceptionInformation[1] : 0));
    logger::log(buf);
    return EXCEPTION_CONTINUE_SEARCH;
}

static DWORD WINAPI MainThread(LPVOID) {
    logger::log("[dos-tool] loaded into process");

    // 1) Headless proof: wait for the engine, then dump live camera/movement state
    //    to the log. Runs regardless of whether the overlay comes up, so it's an
    //    unconditional demonstration that the SDK links + reads real memory.
    if (game::WaitForEngine(300000 /* up to 5 min to reach a level */)) {
        game::Snapshot s = game::Gather();
        logger::log("[proof] engine ready");
        logger::log("[proof] GObjects count : " + std::to_string(s.objectCount));
        logger::log("[proof] world          : " + s.worldName);
        if (s.haveCamera) {
            char buf[128];
            std::snprintf(buf, sizeof(buf), "[proof] camera fov=%.1f dist=%.0f", s.fov, s.distance);
            logger::log(buf);
        }
    } else {
        logger::log("[proof] timed out waiting for UWorld (still on loading screen?)");
    }

    // 2) Install the in-game ImGui overlay (INSERT toggles it).
    overlay::Init();
    logger::log("[dos-tool] init complete; press INSERT in-game for the menu");
    return 0;
}

// Called by the loader (DoS-Tool.asi), see loader.cpp.
extern "C" __declspec(dllexport) void ModStart() {
    g_veh = AddVectoredExceptionHandler(1, CrashRecorder);
    CreateThread(nullptr, 0, (LPTHREAD_START_ROUTINE)MainThread, nullptr, 0, nullptr);
}

extern "C" __declspec(dllexport) void ModStop() {
    overlay::Shutdown();
    if (g_veh) { RemoveVectoredExceptionHandler(g_veh); g_veh = nullptr; }
    logger::log("[dos-tool] unloaded");
}

BOOL APIENTRY DllMain(HMODULE hModule, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH)
        DisableThreadLibraryCalls(hModule);
    return TRUE;
}

#include <Windows.h>
#include <cstdio>
#include <string>

#include "logger.hpp"
#include "game.hpp"
#include "overlay.hpp"

static DWORD WINAPI MainThread(HMODULE self) {
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

BOOL APIENTRY DllMain(HMODULE hModule, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(hModule);
        CreateThread(nullptr, 0, (LPTHREAD_START_ROUTINE)MainThread, hModule, 0, nullptr);
    }
    return TRUE;
}

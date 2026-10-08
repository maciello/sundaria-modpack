#include "probe.hpp"
#include "feature.hpp"
#include "game.hpp"
#include "logger.hpp"
#include "ref.hpp"
#include "umg.hpp"

#include <Windows.h>
#include <atomic>
#include <string>
#include "WidgetMinimap_classes.hpp"

// Dungeon map (#40): the main path as a flowing line on the game's minimap, up to the local player's frontier,
// with blocked icons. Facts: references/game-facts.md § dungeon, game-ui.md § Minimap.
// Dev loop: dungeon-map.probe next to the exe → dos-tool-dungeon.yaml (probe.cpp); watched game events go to the log.
using namespace SDK;

namespace {
    std::atomic<bool> g_on{false}, g_probe{false};
    thread_local bool t_busy = false;
    ref::Fn g_minimapTick{UWidgetMiniMap_C::StaticClass, "WidgetMiniMap_C", "Tick"};
    ref::Ref g_minimap;  // the live HUD minimap, from its own Tick (no search)

    std::string ExeDir() {
        char buf[MAX_PATH] = {};
        GetModuleFileNameA(nullptr, buf, MAX_PATH);
        const std::string p = buf;
        return p.substr(0, p.find_last_of("\\/") + 1);
    }

    void WriteProbe() {
        const std::string out = dungeon_map::probe::Report(g_minimap.Get<void>());
        HANDLE h = CreateFileA((ExeDir() + "dos-tool-dungeon.yaml").c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h == INVALID_HANDLE_VALUE) return;
        DWORD n = 0;
        WriteFile(h, out.data(), DWORD(out.size()), &n, nullptr);
        CloseHandle(h);
        logger::log("[dungeon-map] probe: " + std::to_string(out.size()) + " bytes -> dos-tool-dungeon.yaml");
    }

    void OnEvent(void* obj, void* fn, void*) {
        if (t_busy || !g_on.load(std::memory_order_relaxed) || !game::OnGameThread()) return;
        t_busy = true;
        if (g_minimapTick.Is(fn)) {
            if (!g_minimap.Is(obj) && umg::Live(static_cast<UObject*>(obj))) g_minimap = ref::Ref(obj);
        } else if (umg::IsWorldTick(fn)) {
            if (g_probe.exchange(false)) WriteProbe();
        } else if (const std::string line = dungeon_map::probe::Event(obj, fn); !line.empty()) {
            logger::log(line);
        }
        t_busy = false;
    }

    struct DungeonMap : feature::Feature {
        double next = 0;
        std::string dir;
        DungeonMap() : Feature("Dungeon map", feature::Stage::Alpha) { optIn = true; }  // new game-thread hook
        void OnFrame(const feature::Frame& f) override {
            if (!g_on.exchange(true)) game::SetEventListener(&OnEvent, true);
            if (f.now < next) return;
            next = f.now + 1.0;
            if (dir.empty()) dir = ExeDir();
            const std::string p = dir + "dungeon-map.probe";
            if (GetFileAttributesA(p.c_str()) == INVALID_FILE_ATTRIBUTES) return;
            DeleteFileA(p.c_str());
            g_probe = true;
        }
        void Off() override {
            g_on = false;
            game::SetEventListener(&OnEvent, false);
        }
    } g_feature;
}

#include "feature.hpp"
#include "game.hpp"
#include "../shared/loot.hpp"
#include "imgui.h"

#include <atomic>

// Idle loot sparkle (#27): unlooted items and chests on the floor twinkle in their rarity colour (WoW/Genshin style).
namespace {
    std::atomic<bool> g_on{false};

    void OnEvent(void* obj, void* fn, void*) {
        if (g_on.load(std::memory_order_relaxed)) loot::OnEvent(obj, fn);
    }

    struct IdleLoot : feature::Feature {
        IdleLoot() : Feature("Idle loot sparkle", feature::Stage::Alpha) { optIn = true; }  // new game-thread hook

        void OnFrame(const feature::Frame&) override {
            if (g_on.load()) return;
            g_on = true;
            game::SetEventListener(&OnEvent, true);
        }

        void Off() override {
            g_on = false;
            game::SetEventListener(&OnEvent, false);
            loot::Reset();
        }

        void Menu() override { ImGui::TextDisabled("%d loot actors tracked", loot::Tracked()); }
    } g_idle_loot;
}

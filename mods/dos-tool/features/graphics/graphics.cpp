#include "feature.hpp"
#include "graphics.hpp"
#include "imgui.h"

#include <Windows.h>
#include <atomic>
#include <cstdint>
#include <string>
#include "Engine_classes.hpp"

// Graphics: engine console variables for sharper models (LOD distances, sharpening, anisotropy) and
// smoother frames (motion blur off, less input lag, FPS cap). Console calls are UFunctions, so they
// run on the game thread through core's ProcessEvent listener; the menu only edits kRows.
// Shipping builds reject cheat-flagged cvars: such a row shows "not applied".
using namespace SDK;

namespace {
    using graphics::State;
    using graphics::Step;

    std::atomic<bool> g_on{false}, g_restore{false}, g_restored{false};
    State g_state[graphics::kCount];
    float g_orig[graphics::kCount];
    bool g_haveOrig[graphics::kCount];
    std::atomic<float> g_live[graphics::kCount];  // last value read, for the menu
    ULONGLONG g_lastTick = 0;
    thread_local bool t_busy = false;

    std::wstring Wide(const std::string& s) { return std::wstring(s.begin(), s.end()); }

    float Read(const char* cvar) {
        const std::wstring w = Wide(cvar);
        return UKismetSystemLibrary::GetConsoleVariableFloatValue(FString(w.c_str()));
    }

    void Exec(UWorld* w, const char* cvar, float v) {
        const std::wstring cmd = Wide(graphics::Command(cvar, v));
        UKismetSystemLibrary::ExecuteConsoleCommand(w, FString(cmd.c_str()), nullptr);
    }

    // Game thread, at most twice a second (or at once when restoring).
    void OnEvent(void*, void*, void*) {
        if (t_busy || !g_on.load(std::memory_order_relaxed)) return;
        const bool restore = g_restore.load();
        const ULONGLONG now = GetTickCount64();
        if (!restore && now - g_lastTick < 500) return;
        g_lastTick = now;
        UWorld* w = UWorld::GetWorld();
        if (!w || !w->OwningGameInstance) return;

        t_busy = true;
        for (int i = 0; i < graphics::kCount; i++) {
            const graphics::Row& r = graphics::kRows[i];
            const float cur = Read(r.cvar);
            g_live[i] = cur;
            switch (graphics::Next(r.on && !restore, g_haveOrig[i], g_state[i], cur, r.value)) {
                case Step::None: break;
                case Step::Capture: g_orig[i] = cur; g_haveOrig[i] = true; break;
                case Step::Set:
                    Exec(w, r.cvar, r.value);
                    g_live[i] = Read(r.cvar);
                    g_state[i] = graphics::Same(g_live[i], r.value) ? State::Applied : State::Rejected;
                    break;
                case Step::Restore:
                    Exec(w, r.cvar, g_orig[i]);
                    g_live[i] = Read(r.cvar);
                    g_state[i] = State::Untouched;
                    break;
            }
        }
        t_busy = false;
        if (restore) g_restored = true;
    }

    struct Graphics : feature::Feature {
        Graphics() : Feature("Graphics", feature::Stage::Alpha) { optIn = true; }  // new game-thread hook

        void OnFrame(const feature::Frame&) override {
            if (!g_on) { g_restore = false; g_on = true; game::SetEventListener(&OnEvent, true); }
        }

        // Restore runs on the game thread: ask, wait for it (≤ 1.5 s), then unhook.
        void Off() override {
            if (!g_on) return;
            g_restored = false;
            g_restore = true;
            for (int i = 0; i < 150 && !g_restored; i++) Sleep(10);
            game::SetEventListener(&OnEvent, false);
            g_on = false;
            g_restore = false;
        }

        void Menu() override {
            for (int i = 0; i < graphics::kCount; i++) {
                graphics::Row& r = graphics::kRows[i];
                ImGui::PushID(i);
                ImGui::Checkbox("##on", &r.on);
                ImGui::SameLine(); ImGui::SetNextItemWidth(120);
                ImGui::SliderFloat("##v", &r.value, r.lo, r.hi, "%.2f");
                ImGui::SameLine(); ImGui::TextUnformatted(r.label);
                if (r.on && g_state[i] == State::Rejected) { ImGui::SameLine(); ImGui::TextDisabled("(not applied)"); }
                else if (g_haveOrig[i]) { ImGui::SameLine(); ImGui::TextDisabled("game %.2f  now %.2f", g_orig[i], g_live[i].load()); }
                ImGui::PopID();
            }
            if (ImGui::Button("Retry rejected")) for (State& s : g_state) if (s == State::Rejected) s = State::Untouched;
            ImGui::TextDisabled("Higher detail and shadows cost FPS.");
        }
    } g_graphics;
}

#include "feature.hpp"
#include "logger.hpp"
#include "imgui.h"

#include <Windows.h>
#include <cstdio>
#include <unordered_map>

// Debug probe (dev builds: on when dos-tool.dev exists): logs every health change with ms timestamps
// and the first firing of each game UFunction, so damage events can be matched to HP changes.
namespace {
    bool DevFlag() {
        HMODULE self = nullptr;
        GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           reinterpret_cast<LPCSTR>(&DevFlag), &self);
        char buf[MAX_PATH] = {};
        GetModuleFileNameA(self, buf, MAX_PATH);
        std::string p = buf;
        p = p.substr(0, p.find_last_of("\\/") + 1) + "dos-tool.dev";
        return GetFileAttributesA(p.c_str()) != INVALID_FILE_ATTRIBUTES;
    }

    struct DebugProbe : feature::Feature {
        std::unordered_map<uintptr_t, float> last;
        bool hooked = false;

        DebugProbe() : Feature("Debug probe", false) {}

        void OnFrame(const feature::Frame& f) override {
            if (!hooked) { game::SetEventProbe(true); hooked = true; }
            game::ProbeFlush();
            std::unordered_map<uintptr_t, float> seen;
            for (const combat::Sample& s : f.chars) {
                seen[s.id] = s.health;
                auto it = last.find(s.id);
                if (it == last.end() || it->second == s.health) continue;
                char buf[160];
                std::snprintf(buf, sizeof(buf), "[hp] t=%llu id=%llx %s %.1f -> %.1f (d=%.1f) max=%.0f",
                              GetTickCount64(), (unsigned long long)s.id, s.isPlayer ? "player" : "npc",
                              it->second, s.health, it->second - s.health, s.maxHealth);
                logger::log(buf);
            }
            last.swap(seen);
        }

        void Off() override { game::SetEventProbe(false); hooked = false; last.clear(); }

        void Menu() override { ImGui::TextDisabled("logging to dos-tool.log"); }
    } g_probe;
}

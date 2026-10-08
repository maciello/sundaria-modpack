#include "feature.hpp"
#include "../shared/npc_audio.hpp"
#include "imgui.h"
#include "imgui_internal.h"  // MarkIniSettingsDirty
#include <cstdlib>
#include <string>

// Quiet smith: the hub blacksmith's hammering at a fraction of its volume (default 10%); 0% also stops his
// hammering animation when it plays as a montage.
namespace {
    struct QuietSmith : feature::Feature {
        float volume = 0.1f;
        bool applied = false;

        QuietSmith() : Feature("Quiet smith", feature::Stage::Alpha) {}

        void OnFrame(const feature::Frame&) override {
            if (!applied) { npc_audio::Set("Blacksmith", "Smith", volume); applied = true; }
            npc_audio::Tick();
        }

        void Off() override {
            npc_audio::Set(nullptr, nullptr, 1.0f);
            npc_audio::Tick();
            applied = false;
        }

        void Menu() override {
            ImGui::SetNextItemWidth(150);
            ImGui::SliderFloat("Hammer volume", &volume, 0.0f, 1.0f, "%.2f");
            if (ImGui::IsItemDeactivatedAfterEdit()) { applied = false; ImGui::MarkIniSettingsDirty(); }
        }

        void Load(const char* key, const char* value) override {
            if (std::string(key) == "volume") volume = float(atof(value));
        }
        void Save(std::vector<std::pair<std::string, std::string>>& out) override {
            out.push_back({"volume", std::to_string(volume)});
        }
    } g_quietSmith;
}

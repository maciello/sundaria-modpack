#include "feature.hpp"
#include "logger.hpp"
#include "imgui.h"

#include <Windows.h>
#include <string>
#include <vector>

// Ability dump (dev only): writes dos-tool-abilities.yaml (every loaded ability CDO + the montage playing
// right now) next to the DLL. Agent tool: `just abilities` creates dos-tool-abilities.request, we dump and delete it.
namespace {
    std::string OutPath() {
        HMODULE self = nullptr;
        GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           reinterpret_cast<LPCSTR>(&OutPath), &self);
        char buf[MAX_PATH] = {};
        GetModuleFileNameA(self, buf, MAX_PATH);
        std::string p = buf;
        return p.substr(0, p.find_last_of("\\/") + 1) + "dos-tool-abilities.";
    }

    struct AbilityDump : feature::Feature {
        bool want = false;
        double polled = 0;
        std::string status;

        AbilityDump() : Feature("Ability dump", feature::Stage::Alpha) {}  // Alpha: dev installs only

        // Render thread: memory reads only (game::DumpAbilities does no UFunction calls).
        void OnFrame(const feature::Frame& f) override {
            if (f.now - polled > 0.5) {
                polled = f.now;
                if (DeleteFileA((OutPath() + "request").c_str())) want = true;
            }
            if (!want) return;
            want = false;
            std::vector<ability_dump::Ability> abilities;
            ability_dump::Live live;
            game::DumpAbilities(abilities, live);
            const std::string yaml = ability_dump::ToYaml(abilities, live);
            const std::string path = OutPath() + "yaml";
            HANDLE h = CreateFileA(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
            DWORD w = 0;
            const bool ok = h != INVALID_HANDLE_VALUE && WriteFile(h, yaml.data(), (DWORD)yaml.size(), &w, nullptr);
            if (h != INVALID_HANDLE_VALUE) CloseHandle(h);
            status = (ok ? "wrote " : "FAILED ") + std::to_string(abilities.size()) + " abilities, live=" + (live.valid ? live.ability : "none");
            logger::log("[ability-dump] " + status + " -> " + path);
        }

        void Menu() override {
            if (ImGui::Button("Dump abilities")) want = true;
            if (!status.empty()) ImGui::TextDisabled("%s", status.c_str());
        }
    } g_ability_dump;
}

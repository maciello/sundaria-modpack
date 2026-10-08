#include "feature.hpp"
#include "draw.hpp"
#include "imgui.h"

namespace {
    struct DpsMeter : feature::Feature {
        DpsMeter() : Feature("DPS meter", true) {}

        void OnFrame(const feature::Frame& fr) override {
            const combat::Tracker& c = fr.combat;
            if (!c.inFight) return;
            const combat::Fight& f = c.fight;
            const bool active = c.FightActive(fr.now);
            char dps[24], total[24];
            draw::FormatAmount(dps, sizeof(dps), f.Dps());
            draw::FormatAmount(total, sizeof(total), f.total);
            ImGui::SetNextWindowPos(ImVec2(fr.w - 16, 16), ImGuiCond_Always, ImVec2(1, 0));
            ImGui::SetNextWindowBgAlpha(0.35f);
            ImGui::Begin("##dps", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoInputs |
                         ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav);
            ImGui::PushFont(fr.font);
            ImGui::SetWindowFontScale(0.4f);
            ImGui::TextColored(active ? ImVec4(1, 0.85f, 0.4f, 1) : ImVec4(0.7f, 0.7f, 0.7f, 1), "DPS %s", dps);
            ImGui::SetWindowFontScale(0.28f);
            ImGui::Text("total %s  |  %.0fs", total, f.Duration());
            ImGui::PopFont();
            ImGui::End();
        }
    } g_dps;
}

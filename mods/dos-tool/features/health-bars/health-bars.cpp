#include "feature.hpp"
#include "health-bars.hpp"
#include "imgui.h"

// Health bars: enemy HP bar above the head, shown once damaged, with a trailing chip on hits.
namespace {
    struct HealthBars : feature::Feature {
        health_bars::Bars bars;
        float height = 110.0f;  // cm above capsule center
        float width = 1.0f;
        double last = 0;

        HealthBars() : Feature("Health bars", true) {}

        void OnFrame(const feature::Frame& f) override {
            const double dt = last > 0 ? f.now - last : 0.0;
            last = f.now;
            const std::vector<health_bars::Bar> v = bars.Update(f.chars, f.now, dt);
            combat::View view;
            if (v.empty() || !game::GetView(view)) return;
            ImDrawList* dl = ImGui::GetBackgroundDrawList();
            const float w = f.h / 14.0f * width, h = std::max(4.0f, f.h / 180.0f);
            for (const health_bars::Bar& b : v) {
                float sx, sy;
                if (!combat::Project(view, b.x, b.y, b.z + height, f.w, f.h, sx, sy)) continue;
                const ImVec2 a(sx - w * 0.5f, sy - h * 0.5f), z(sx + w * 0.5f, sy + h * 0.5f);
                dl->AddRectFilled(ImVec2(a.x - 1, a.y - 1), ImVec2(z.x + 1, z.y + 1), IM_COL32(10, 8, 6, 200), 2.0f);
                dl->AddRectFilled(a, ImVec2(a.x + w * b.chip, z.y), IM_COL32(255, 230, 190, 220), 2.0f);
                const int g = int(60 + 140 * b.frac);
                dl->AddRectFilled(a, ImVec2(a.x + w * b.frac, z.y), IM_COL32(220, g, 50, 255), 2.0f);
            }
        }

        void Off() override { bars.st.clear(); last = 0; }

        void Menu() override {
            ImGui::SliderFloat("Bar height", &height, 40.0f, 250.0f, "%.0f cm");
            ImGui::SliderFloat("Bar width", &width, 0.5f, 2.0f, "%.2fx");
        }
    } g_health_bars;
}

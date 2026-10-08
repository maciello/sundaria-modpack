#include "feature.hpp"
#include "health-bars.hpp"
#include "draw.hpp"
#include "imgui.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

// Health bars: Genshin-style enemy HP bar floating over the head once the enemy is hit and below
// full HP: slim rounded bar, red fill with a light top edge, white chip trail, hit flash, "Lv.N", depth scaling.
namespace {
    ImU32 Col(float r, float g, float b, float a) { return IM_COL32(int(r), int(g), int(b), int(std::clamp(a, 0.0f, 255.0f))); }

    struct HealthBars : feature::Feature {
        health_bars::Bars bars;
        float height = 100.0f;  // cm above capsule center
        float width = 0.6f;
        bool showLevel = true;
        double last = 0;

        HealthBars() : Feature("Health bars", feature::Stage::Stable) {}

        void OnFrame(const feature::Frame& f) override {
            const double dt = last > 0 ? f.now - last : 0.0;
            last = f.now;
            std::vector<health_bars::Bar> v = bars.Update(f.chars, f.now, dt);
            combat::View view;
            if (v.empty() || !game::GetView(view)) return;
            auto dist = [&](const health_bars::Bar& b) {
                return std::sqrt((b.x - view.x) * (b.x - view.x) + (b.y - view.y) * (b.y - view.y) + (b.z - view.z) * (b.z - view.z));
            };
            std::sort(v.begin(), v.end(), [&](const auto& a, const auto& b) { return dist(a) > dist(b); });  // near bars on top
            ImDrawList* dl = ImGui::GetBackgroundDrawList();
            const float ui = f.h / 1080.0f;
            for (const health_bars::Bar& b : v) {
                float sx, sy;
                if (!combat::Project(view, b.x, b.y, b.z + height, f.w, f.h, sx, sy)) continue;
                const float depth = std::clamp(1500.0f / std::max(dist(b), 1.0f), 0.6f, 1.25f);
                const float pop = 0.92f + 0.08f * b.alpha;
                const float w = 118.0f * ui * width * depth * pop, h = std::max(5.0f, 8.0f * ui * depth * pop);
                const float A = 255.0f * b.alpha, r = h * 0.5f;
                const ImVec2 a(sx - w * 0.5f, sy - h * 0.5f), z(sx + w * 0.5f, sy + h * 0.5f);

                dl->AddRectFilled(ImVec2(a.x - 2, a.y - 2), ImVec2(z.x + 2, z.y + 2), Col(12, 8, 8, A * 0.75f), r + 2);  // outline
                dl->AddRectFilled(a, z, Col(45, 32, 32, A * 0.6f), r);                                                // empty track
                dl->PushClipRect(a, ImVec2(a.x + w * b.chip, z.y), true);
                dl->AddRectFilled(a, z, Col(255, 248, 235, A * 0.9f), r);                                             // chip trail
                dl->PopClipRect();
                dl->PushClipRect(a, ImVec2(a.x + w * b.frac, z.y), true);
                const float fl = b.flash * 0.7f;
                dl->AddRectFilled(a, z, Col(215 + 40 * fl, 45 + 210 * fl, 40 + 215 * fl, A), r);                       // fill
                dl->AddRectFilled(a, ImVec2(z.x, a.y + h * 0.45f), Col(255, 140, 120, A * 0.35f), r);                   // top sheen
                dl->PopClipRect();

                if (showLevel && b.level > 0) {
                    char lv[16];
                    std::snprintf(lv, sizeof(lv), "Lv.%d", int(b.level));
                    const float ts = std::max(13.0f, 17.0f * ui * depth);
                    const ImVec2 sz = f.font->CalcTextSizeA(ts, FLT_MAX, 0.0f, lv);
                    draw::OutlinedText(dl, f.font, ts, ImVec2(a.x - sz.x - 6 * ui, sy - sz.y * 0.5f),
                                       Col(255, 255, 255, A), Col(12, 8, 8, A * 0.8f), std::max(1.0f, ts / 14.0f), lv);
                }
            }
        }

        void Off() override { bars.st.clear(); last = 0; }

        void Menu() override {
            ImGui::SliderFloat("Bar height", &height, 40.0f, 250.0f, "%.0f cm");
            ImGui::SliderFloat("Bar width", &width, 0.5f, 2.0f, "%.2fx");
            ImGui::Checkbox("Show level", &showLevel);
        }
    } g_health_bars;
}

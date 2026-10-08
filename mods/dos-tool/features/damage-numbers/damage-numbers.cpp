#include "feature.hpp"
#include "draw.hpp"
#include "anim.hpp"
#include "colors.hpp"
#include "imgui.h"

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstring>
#include <utility>
#include <vector>

namespace {
    // Element badge from draw-list primitives: dark disc, glyph in the element colour. c = centre, R = radius.
    void DrawIcon(ImDrawList* dl, combat::Element e, ImVec2 c, float R, ImU32 col, ImU32 back) {
        using E = combat::Element;
        dl->AddCircleFilled(c, R, back);
        const float r = R * 0.68f, t = std::max(1.0f, R * 0.22f);
        auto P = [&](float x, float y) { return ImVec2(c.x + x * r, c.y + y * r); };
        switch (e) {
            case E::Fire:  // flame: round base, pointed tip
                dl->AddCircleFilled(P(0, 0.35f), r * 0.62f, col);
                dl->AddTriangleFilled(P(-0.6f, 0.2f), P(0.6f, 0.2f), P(0.1f, -1.0f), col);
                break;
            case E::Ice:  // snowflake: three crossing bars
                for (int i = 0; i < 3; i++) {
                    const float a = 3.14159265f / 3 * i, x = std::cos(a), y = std::sin(a);
                    dl->AddLine(P(-x, -y), P(x, y), col, t);
                }
                break;
            case E::Lightning: {  // bolt
                const ImVec2 pts[] = {P(0.35f, -1), P(-0.4f, 0.1f), P(0.25f, 0.1f), P(-0.35f, 1)};
                dl->AddPolyline(pts, 4, col, 0, t * 1.2f);
                break;
            }
            case E::Holy:  // four-point star
                dl->AddQuadFilled(P(0, -1), P(0.28f, 0), P(0, 1), P(-0.28f, 0), col);
                dl->AddQuadFilled(P(-1, 0), P(0, -0.28f), P(1, 0), P(0, 0.28f), col);
                break;
            case E::Poison:  // bubbles
                dl->AddCircleFilled(P(-0.3f, 0.35f), r * 0.5f, col);
                dl->AddCircleFilled(P(0.45f, 0.1f), r * 0.35f, col);
                dl->AddCircleFilled(P(0.05f, -0.55f), r * 0.28f, col);
                break;
            case E::Shadow:  // crescent
                dl->AddCircleFilled(c, r, col);
                dl->AddCircleFilled(P(0.45f, -0.3f), r * 0.85f, back);
                break;
            case E::Arcane:  // diamond
                dl->AddQuadFilled(P(0, -1), P(0.7f, 0), P(0, 1), P(-0.7f, 0), col);
                break;
            case E::Environment:  // warning triangle
                dl->AddTriangle(P(0, -0.95f), P(0.95f, 0.75f), P(-0.95f, 0.75f), col, t);
                break;
            default: break;
        }
    }

    struct DamageNumbers : feature::Feature {
        float height = 40.0f;  // cm above capsule center
        float size = 1.0f;
        float maxStack = 1.6f;  // stacked numbers grow at most to this × a typical hit
        bool icons = true;     // element badge on non-physical numbers
        struct Preview { float sx, sy; combat::Number n; std::vector<std::pair<double, float>> ticks; };
        std::vector<Preview> preview;
        float previewTypical = 20;
        double lifetime = 1.4;

        DamageNumbers() : Feature("Damage numbers", feature::Stage::Stable) {}

        void DrawNumber(ImDrawList* dl, ImFont* font, float base, float sx, float sy, const combat::Number& n, double now) {
            const double sinceBorn = now - n.born, sinceBump = now - n.bump;
            if (sinceBorn < 0) return;
            const float scale = combat::Shown(n, maxStack);
            const float big = std::clamp((scale - 1.0f) / 0.8f, 0.0f, 1.0f);
            const dmgnum::Anim an = dmgnum::Animate(sinceBorn, sinceBump, lifetime, n.drift,
                                                    n.kind == combat::Kind::Dealt ? big : 0.0f, n.hits > 1);
            if (an.scale <= 0.01f || an.alpha <= 0.0f) return;
            char buf[24];
            draw::FormatAmount(buf, sizeof(buf) - 1, n.amount);
            float r, g, b;
            switch (n.kind) {
                case combat::Kind::Heal:  r = 110; g = 255; b = 140; std::memmove(buf + 1, buf, strlen(buf) + 1); buf[0] = '+'; break;
                case combat::Kind::Taken: r = 255; g = 80;  b = 70;  break;
                default:
                    if (n.element == combat::Element::Physical) { r = 255; g = 255 - 70 * big; b = 255 - 200 * big; }  // white -> gold
                    else { const dmgnum::Rgb c = dmgnum::ColorOf(n.element); r = c.r; g = c.g; b = c.b; }
            }
            r += (255 - r) * an.flash; g += (255 - g) * an.flash; b += (255 - b) * an.flash;  // impact flash
            const int a = int(255 * an.alpha);
            const float unit = base * size * scale;
            const float sz = unit * an.scale;
            const ImVec2 ts = font->CalcTextSizeA(sz, FLT_MAX, 0.0f, buf);
            const ImVec2 pos(sx + an.dx * unit - ts.x * 0.5f, sy + an.dy * unit - ts.y * 0.5f);
            const float ow = std::max(1.5f, sz / 16.0f);
            const ImU32 outline = IM_COL32(20, 12, 8, int(a * 0.85f));
            if (an.flash > 0)  // soft glow while hot
                draw::OutlinedText(dl, font, sz, pos, IM_COL32(0, 0, 0, 0), IM_COL32(int(r), int(g), int(b), int(90 * an.flash * an.alpha)), ow * 3.0f, buf);
            draw::OutlinedText(dl, font, sz, pos, IM_COL32(int(r), int(g), int(b), a), outline, ow, buf);
            if (icons && n.element != combat::Element::Physical && n.kind != combat::Kind::Heal) {
                const dmgnum::Rgb c = dmgnum::ColorOf(n.element);
                const float R = std::max(6.0f, sz * 0.2f);
                DrawIcon(dl, n.element, ImVec2(pos.x + ts.x + 2 + R, pos.y + R * 0.6f), R, IM_COL32(int(c.r), int(c.g), int(c.b), a), outline);
            }
            if (n.hits > 1) {  // hit counter: small, bottom-right, on the number's baseline
                char cnt[16];
                std::snprintf(cnt, sizeof(cnt), "x%d", n.hits);
                const float cs = std::max(12.0f, sz * 0.34f);
                const float ch = font->CalcTextSizeA(cs, FLT_MAX, 0.0f, cnt).y;
                draw::OutlinedText(dl, font, cs, ImVec2(pos.x + ts.x + 2, pos.y + ts.y - ch), IM_COL32(255, 210, 120, a), outline, std::max(1.0f, cs / 14.0f), cnt);
            }
        }

        void OnFrame(const feature::Frame& f) override {
            lifetime = f.combat.lifetime;
            const float base = f.h / 26.0f;  // ~42 px at 1080p for a typical hit
            ImDrawList* dl = ImGui::GetForegroundDrawList();
            combat::Tracker rel;
            rel.typical = previewTypical;
            for (Preview& p : preview)
                while (!p.ticks.empty() && p.ticks.front().first <= f.now) {
                    p.n.amount += p.ticks.front().second;
                    p.n.hits++;
                    p.n.bump = p.ticks.front().first;
                    p.n.scale = rel.Grow(p.n.amount);
                    p.ticks.erase(p.ticks.begin());
                }
            std::erase_if(preview, [&](const Preview& p) { return p.ticks.empty() && f.now - p.n.bump > lifetime; });
            for (const Preview& p : preview) DrawNumber(dl, f.font, base, p.sx, p.sy, p.n, f.now);
            combat::View view;
            if (f.combat.live.empty() || !game::GetView(view)) return;
            for (const combat::Number& n : f.combat.live) {
                float sx, sy;
                if (combat::Project(view, n.x, n.y, n.z + height, f.w, f.h, sx, sy))
                    DrawNumber(dl, f.font, base, sx, sy, n, f.now);
            }
        }

        void Off() override { preview.clear(); }

        void Menu() override {
            ImGui::SliderFloat("Number height", &height, -60.0f, 160.0f, "%.0f cm");
            ImGui::SliderFloat("Number size", &size, 0.5f, 2.0f, "%.2fx");
            ImGui::SliderFloat("Max stack size", &maxStack, 1.0f, 2.4f, "%.1fx");
            ImGui::Checkbox("Element icons", &icons);
            if (!ImGui::Button("Preview numbers")) return;
            const ImVec2 c(ImGui::GetIO().DisplaySize.x * 0.5f, ImGui::GetIO().DisplaySize.y * 0.45f);
            const double now = ImGui::GetTime();
            const float amounts[] = {18, 24, 21, 95, 19, 260};
            using E = combat::Element;
            const E elements[] = {E::Holy, E::Fire, E::Ice, E::Shadow, E::Lightning, E::Physical};
            combat::Tracker scratch;  // don't let fake hits move the real "typical hit"
            scratch.typical = 20;
            for (int i = 0; i < 6; i++) {
                const float sc = scratch.Scale(amounts[i]);
                preview.push_back({c.x + (i - 2.5f) * 50.0f, c.y, {0, 0, 0, amounts[i], combat::Kind::Dealt, sc, (i % 3) - 1.0f, now + i * 0.18, 0, 1, now + i * 0.18, 1, elements[i]}});
            }
            preview.push_back({c.x - 220, c.y + 80, {0, 0, 0, 35, combat::Kind::Taken, 0.95f, -1, now + 0.4, 0, 1, now + 0.4, 1, E::Poison}});
            preview.push_back({c.x + 220, c.y + 80, {0, 0, 0, 50, combat::Kind::Heal, 0.9f, 1, now + 0.7}});
            Preview dot{c.x, c.y + 170, {0, 0, 0, 12, combat::Kind::Dealt, scratch.Rel(12), 0.3f, now + 1.2, 0, 1, now + 1.2, 1, E::Poison}};  // DoT: 8 ticks stack
            for (int i = 1; i < 8; i++) dot.ticks.push_back({now + 1.2 + i * 0.45, 12.0f + i});
            preview.push_back(dot);
            previewTypical = scratch.typical;
        }
    } g_numbers;
}

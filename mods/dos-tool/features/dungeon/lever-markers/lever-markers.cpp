#include "marker.hpp"
#include "../shared/planner.hpp"
#include "feature.hpp"
#include "game.hpp"
#include "style.hpp"
#include "draw.hpp"
#include "imgui.h"

#include <algorithm>
#include <cfloat>
#include <cstdio>
#include <vector>

// Lever markers (#93): over each unpulled lever that opens the main route's way (a locked door ahead), a yellow diamond
// with its distance; off screen it sits on the screen edge with a chevron towards it. Render thread only: reads the
// planner's plain copies (shared/planner.hpp) and the camera. Spec: design-system.md § Dungeon map path (levers).
namespace {
    using dungeon_map::V3;

    void DrawLever(ImDrawList* dl, const feature::Frame& f, const combat::View& view, V3 at, float dist, float alpha) {
        using namespace style;
        const float ui = type::Ui(f.h), r = dungeon_map::kMarkerPx * ui * 0.5f, o = stroke::Outline(type::kSm * ui);
        const dungeon_map::Marker m = dungeon_map::Place(view, {at.x, at.y, at.z + dungeon_map::kLeverLift}, f.w, f.h, space::k7 * ui);
        auto diamond = [&](float rr, ImU32 c) { dl->AddQuadFilled({m.x, m.y - rr}, {m.x + rr, m.y}, {m.x, m.y + rr}, {m.x - rr, m.y}, c); };
        diamond(r + o, Pack(color::kInk, alpha));
        diamond(r, Pack(color::kGameHighlight, alpha));
        if (m.edge) {  // chevron: two bars meeting at a tip beyond the diamond, towards the lever
            const float c = std::cos(m.angle), s = std::sin(m.angle), len = dungeon_map::kChevronLen * ui;
            const float reach = r + o + space::k3 * ui + len * 0.7f;
            const ImVec2 tip{m.x + c * reach, m.y + s * reach};
            for (float side : {0.785f, -0.785f}) {
                const float a = m.angle + 3.14159265f + side;
                dl->AddLine(tip, {tip.x + std::cos(a) * len, tip.y + std::sin(a) * len}, Pack(color::kGameHighlight, alpha), dungeon_map::kChevronW * ui);
            }
            return;
        }
        char b[16];
        std::snprintf(b, sizeof b, "%.0f m", dist / 100);
        const float size = type::kSm * ui;
        const ImVec2 ts = f.font->CalcTextSizeA(size, FLT_MAX, 0, b);
        draw::OutlinedText(dl, f.font, size, {m.x - ts.x * 0.5f, m.y + r + o + space::k2 * ui}, Pack(color::kTextSoft, alpha),
                           Pack(color::kInk, alpha * stroke::kOutlineAlpha), o, b);
    }

    struct LeverMarkers : feature::Feature {
        struct Fade { V3 at; float a = 0; bool want = false; };
        std::vector<Fade> fades;  // one per marker
        double last = 0;

        LeverMarkers() : Feature("Lever markers", feature::Stage::Alpha) { optIn = true; }  // new game-thread hook (planner)
        void OnFrame(const feature::Frame& f) override {
            dungeon_map::planner::Use(dungeon_map::planner::kLevers, true);
            const float dt = last > 0 ? float(std::min(f.now - last, 0.1)) : 0;
            last = f.now;
            V3 pawn;
            std::vector<V3> ls;
            dungeon_map::planner::Levers(pawn, ls);
            for (Fade& x : fades) x.want = false;
            for (const V3& l : ls) {
                auto it = std::find_if(fades.begin(), fades.end(), [&](const Fade& x) { return dungeon_map::Dist(x.at, l) < 50; });
                if (it == fades.end()) it = fades.insert(fades.end(), Fade{l});
                const float d = dungeon_map::Dist(pawn, l);
                it->want = d > dungeon_map::kLeverNear && d <= dungeon_map::kLeverRange;
            }
            for (Fade& x : fades)
                x.a = std::clamp(x.a + (x.want ? dt / style::motion::kFadeIn.dur : -dt / style::motion::kFadeOut.dur), 0.f, 1.f);
            std::erase_if(fades, [](const Fade& x) { return x.a <= 0 && !x.want; });
            combat::View view;
            if (fades.empty() || !game::GetView(view)) return;
            ImDrawList* dl = ImGui::GetBackgroundDrawList();  // Layer::WorldBar
            for (const Fade& x : fades) {
                const auto& c = x.want ? style::motion::kFadeIn : style::motion::kFadeOut;
                DrawLever(dl, f, view, x.at, dungeon_map::Dist(pawn, x.at), style::ease::Apply(c.curve, x.a));
            }
        }
        void Off() override {  // ImGui only: nothing in the world to remove
            dungeon_map::planner::Use(dungeon_map::planner::kLevers, false);
            fades.clear();
            last = 0;
        }
    } g_feature;
}

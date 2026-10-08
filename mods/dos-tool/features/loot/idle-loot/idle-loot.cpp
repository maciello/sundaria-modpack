#include "feature.hpp"
#include "game.hpp"
#include "logger.hpp"
#include "cost.hpp"
#include "idle-loot.hpp"
#include "../shared/loot.hpp"
#include "imgui.h"

#include <atomic>
#include <cmath>
#include <cstdio>
#include <unordered_map>
#include <vector>

// Idle loot sparkle (#27): unlooted items and chests on the floor twinkle (WoW/Genshin style): a breathing glow in
// the game's own grade colour and short star glints, white-hot core in a tier-coloured star. Stops (fades) within
// kFadeOut of looting; hidden behind walls and beyond 30 m. Overlay only: the game's world is not touched.
// Spec: references/design-system.md § Loot marker (idle shimmer).
using namespace idle_loot;

namespace {
    std::atomic<bool> g_on{false};

    void OnEvent(void* obj, void* fn, void*) {
        if (g_on.load(std::memory_order_relaxed)) loot::OnEvent(obj, fn);
    }

    struct Mark { float alpha = 0, dist = 0; bool unlooted = false, alive = false, shown = false; };

    // Arcane sparkle glyph (design-system § Element icon): two thin diamonds, r = half length.
    void Star(ImDrawList* dl, ImVec2 c, float r, ImU32 col) {
        const float w = 0.28f * r;
        dl->AddQuadFilled({c.x, c.y - r}, {c.x + w, c.y}, {c.x, c.y + r}, {c.x - w, c.y}, col);
        dl->AddQuadFilled({c.x - r, c.y}, {c.x, c.y - w}, {c.x + r, c.y}, {c.x, c.y + w}, col);
    }

    struct IdleLoot : feature::Feature {
        std::vector<loot::Actor> actors;
        std::unordered_map<std::uintptr_t, Mark> marks;  // key = actor address; entries leave once faded out
        double last = 0;

        IdleLoot() : Feature("Idle loot sparkle", feature::Stage::Alpha) { optIn = true; }  // new game-thread hook

        void OnFrame(const feature::Frame& f) override {
            if (!g_on.load()) {
                g_on = true;
                game::SetEventListener(&OnEvent, true);
            }
            const float dt = last > 0 ? float(std::min(f.now - last, 0.1)) : 0.0f;
            last = f.now;
            combat::View view;
            if (!game::GetView(view)) return;
            static cost::Path path{"idle-loot frame"};
            cost::Scope cs(path);

            actors.clear();
            loot::Read(view.x, view.y, view.z, kCull, actors);
            float newest = 0;  // the game's newest render time: the player's own mesh is on screen
            for (const combat::Sample& c : f.chars) newest = std::max(newest, c.seen);
            std::array<style::Rgba, 8> tier;
            if (!loot::GradeColors(tier))
                for (int g = 0; g < 8; g++) tier[g] = style::rarity::Of(g);

            for (auto& [id, m] : marks) m.alive = false;
            ImDrawList* dl = ImGui::GetBackgroundDrawList();  // Layer::WorldGlow
            const float ui = style::type::Ui(f.h);
            for (const loot::Actor& a : actors) {
                Mark& m = marks[a.id];
                m.alive = true;
                if (m.unlooted && !a.unlooted) logger::log(Stopped(a));
                m.unlooted = a.unlooted;
                const float dx = a.x - view.x, dy = a.y - view.y, dz = a.z - view.z;
                const float dist = m.dist = std::sqrt(dx * dx + dy * dy + dz * dz);
                m.alpha = Approach(m.alpha, a.unlooted && OnScreen(a.seen, newest) ? 1.0f : 0.0f, dt);
                if (m.alpha <= 0) continue;
                if (!m.shown) {
                    m.shown = true;
                    char b[120];
                    std::snprintf(b, sizeof b, "[idle-loot] cue starts: %s %llx grade %d at %.0f m", a.kind == loot::Kind::Chest ? "chest" : "item",
                                  (unsigned long long)a.id, a.grade, dist / 100);
                    logger::log(b);
                }

                const float lift = a.kind == loot::Kind::Chest ? kLiftChest : kLiftItem;
                float gx, gy, sx, sy;
                if (!combat::Project(view, a.x, a.y, a.z, f.w, f.h, gx, gy) || !combat::Project(view, a.x, a.y, a.z + lift, f.w, f.h, sx, sy))
                    continue;
                const float u = ui * Depth(dist);
                const float alpha = style::ease::Apply(style::ease::Curve::OutCubic, m.alpha) * DistFade(dist);
                const bool known = a.grade >= 0 && a.grade < 8;
                const style::Rgba col = known ? tier[a.grade] : style::color::kTextSoft;  // chest contents unknown until opened

                // Breathing glow on the floor (alpha only, never scale): from kGlowFrom up, and on chests.
                if (!known || a.grade >= style::rarity::kGlowFrom) {
                    const float b = Breathe(f.now, Phase(a.id));
                    for (int i = 2; i >= 0; i--) dl->AddCircleFilled({gx, gy}, kGlowR[i] * u, style::Pack(col, kGlowA[i] * b * alpha), 24);
                }
                // Glints: tier-coloured halo + star, white-hot core.
                for (int k = 0; k < kGlints; k++) {
                    const Glint g = GlintAt(a.id, f.now, k);
                    if (g.scale <= 0) continue;
                    const ImVec2 c{sx + g.dx * u, sy + g.dy * u};
                    const float r = kGlintR * u * g.scale;
                    dl->AddCircleFilled(c, r * kHaloScale * 0.5f, style::Pack(col, style::stroke::kGlowAlpha * alpha), 12);
                    Star(dl, c, r, style::Pack(col, alpha));
                    Star(dl, c, r * kCoreScale, style::Pack(style::color::kText, alpha));
                }
            }
            std::erase_if(marks, [](const auto& kv) {
                const Mark& m = kv.second;
                if (!m.alive && m.unlooted && m.dist < kFadeFrom)  // destroyed on pickup (still in range, so not culled)
                    logger::log("[idle-loot] cue stops: " + std::to_string(kv.first) + " removed by the game");
                return !m.alive || (m.alpha <= 0 && !m.unlooted);
            });
        }

        static std::string Stopped(const loot::Actor& a) {
            char b[120];
            std::snprintf(b, sizeof b, "[idle-loot] cue stops: %s %llx looted (fade %.1f s)", a.kind == loot::Kind::Chest ? "chest" : "item",
                          (unsigned long long)a.id, style::motion::kFadeOut.dur);
            return b;
        }

        void Off() override {
            g_on = false;
            game::SetEventListener(&OnEvent, false);
            loot::Reset();
            marks.clear();
            last = 0;
        }
    } g_idle_loot;
}

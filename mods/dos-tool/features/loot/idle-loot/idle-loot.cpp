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

// Idle loot sparkle (#27): WoW lootable-corpse style. Each pile of unlooted loot (items within kPileR, or a chest)
// now and then lets off a small burst of tiny rising motes, at random intervals, in the game's own colour of the best
// item grade in the pile. Hidden behind walls and beyond 30 m. Overlay: no game particle system takes a colour
// (game-facts.md § loot_fx). Spec: references/design-system.md § Loot marker (idle sparkle).
using namespace idle_loot;

namespace {
    std::atomic<bool> g_on{false};

    void OnEvent(void* obj, void* fn, void*) {
        if (g_on.load(std::memory_order_relaxed)) loot::OnEvent(obj, fn);
    }

    struct Mark {
        float x = 0, y = 0, z = 0, alpha = 0;
        int grade = -1, members = 0;
        bool chest = false, alive = false;
        double next = -1, burstAt = -1e9;  // next burst, start of the current one
        std::uint32_t burst = 0;
    };

    // Arcane sparkle glyph (design-system § Element icon): two thin diamonds, r = half length.
    void Star(ImDrawList* dl, ImVec2 c, float r, ImU32 col) {
        const float w = 0.28f * r;
        dl->AddQuadFilled({c.x, c.y - r}, {c.x + w, c.y}, {c.x, c.y + r}, {c.x - w, c.y}, col);
        dl->AddQuadFilled({c.x - r, c.y}, {c.x, c.y - w}, {c.x + r, c.y}, {c.x, c.y + w}, col);
    }

    std::string Describe(const char* what, std::uint64_t key, const Mark& m, float dist) {
        char b[160];
        std::snprintf(b, sizeof b, "[idle-loot] %s: %s %llx, %d item(s), best grade %d at %.0f m", what, m.chest ? "chest" : "pile",
                      (unsigned long long)key, m.members, m.grade, dist / 100);
        return b;
    }

    // ReceiveBeginPlay of every class (each override is its own UFunction, same name) + the world tick.
    void Listen(bool on) {
        game::On(nullptr, "ReceiveBeginPlay", &OnEvent, on);
        game::OnWorldTick(&OnEvent, on);
    }

    struct IdleLoot : feature::Feature {
        std::vector<loot::Actor> actors;
        std::vector<Pile> piles;
        std::unordered_map<std::uint64_t, Mark> marks;  // key = Pile::key; a vanished pile leaves once its burst is over
        double last = 0;

        IdleLoot() : Feature("Idle loot sparkle", feature::Stage::Alpha) { optIn = true; usesCombat = true; }  // new game-thread hook

        void OnFrame(const feature::Frame& f) override {
            if (!g_on.load()) {
                g_on = true;
                Listen(true);
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
            Piles(actors, newest, piles);
            std::array<style::Rgba, 8> tier;
            if (!loot::GradeColors(tier))
                for (int g = 0; g < 8; g++) tier[g] = style::rarity::Of(g);

            for (auto& [k, m] : marks) m.alive = false;
            for (const Pile& p : piles) {
                Mark& m = marks[p.key];
                const bool fresh = m.next < 0;
                m.alive = true;
                m.x = p.x, m.y = p.y, m.z = p.z, m.grade = p.grade, m.chest = p.chest, m.members = p.members;
                m.alpha = Approach(m.alpha, p.onScreen ? 1.0f : 0.0f, dt);
                if (fresh) {
                    m.next = f.now + Gap(p.key, 0);
                    logger::log(Describe("cue starts", p.key, m, Dist(view, m)));
                } else if (f.now >= m.next) {  // hidden piles skip their burst, the schedule runs on
                    if (m.alpha > 0) m.burstAt = f.now;
                    m.next = f.now + Gap(p.key, ++m.burst);
                }
            }

            ImDrawList* dl = ImGui::GetBackgroundDrawList();  // Layer::WorldGlow
            const float ui = style::type::Ui(f.h);
            for (const auto& [key, m] : marks) {
                const float age = float(f.now - m.burstAt);
                if (age >= kBurstLen || m.alpha <= 0) continue;
                const float dist = Dist(view, m);
                float sx, sy;
                if (!combat::Project(view, m.x, m.y, m.z + (m.chest ? kLiftChest : kLiftItem), f.w, f.h, sx, sy)) continue;
                const float u = ui * Depth(dist);
                const float alpha = style::ease::Apply(style::ease::Curve::OutCubic, m.alpha) * DistFade(dist);
                const bool known = m.grade >= 0 && m.grade < 8;
                const style::Rgba col = known ? tier[m.grade] : style::color::kText;  // chest contents unknown until opened
                const style::Rgba core = style::Mix(col, style::color::kText, kCoreWhite);
                for (int k = 0, n = Motes(key, m.burst); k < n; k++) {
                    const Mote mo = MoteAt(key, m.burst, k, age);
                    if (mo.a <= 0) continue;
                    const ImVec2 c{sx + mo.dx * u, sy + mo.dy * u};
                    const float r = mo.r * u, a = mo.a * alpha;
                    dl->AddCircleFilled(c, r * kHaloScale, style::Pack(col, kHaloA * a), 10);
                    if (mo.glint) {
                        Star(dl, c, r, style::Pack(col, a));
                        Star(dl, c, r * 0.5f, style::Pack(style::color::kText, a));
                    } else {
                        dl->AddCircleFilled(c, r, style::Pack(core, a), 8);
                    }
                }
            }
            std::erase_if(marks, [&](auto& kv) {  // looted / streamed out / out of range
                Mark& m = kv.second;
                if (m.alive) return false;
                if (m.next >= 0) logger::log(Describe("cue stops", kv.first, m, Dist(view, m)));
                m.next = -2;  // logged once; erased when its last burst is over
                return f.now - m.burstAt >= kBurstLen;
            });
        }

        static float Dist(const combat::View& v, const Mark& m) {
            const float dx = m.x - v.x, dy = m.y - v.y, dz = m.z - v.z;
            return std::sqrt(dx * dx + dy * dy + dz * dz);
        }

        void Off() override {
            g_on = false;
            Listen(false);
            loot::Reset();
            marks.clear();
            last = 0;
        }
    } g_idle_loot;
}

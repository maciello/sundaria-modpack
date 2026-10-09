#include "feature.hpp"
#include "cast-indicator.hpp"
#include "logger.hpp"
#include "montage.hpp"
#include "style.hpp"
#include "trace.hpp"
#include "umg.hpp"
#include "imgui.h"

#include <Windows.h>
#include <atomic>
#include <cstdint>
#include <algorithm>
#include <cstdio>
#include <string>
#include <vector>

// Cast indicator (#3): while the local hero plays an ability montage with 2+ hit notifies, a row of pips under
// the character, one per hit the montage should land, filling per landed hit. A cast whose first hit comes after a
// wind-up gets a ring shrinking onto the first pip that meets it when the hit fires (#92).
//   N      = ApplyEffect/ShootProjectile notifies (UBP_GameplayAnimNotify_C) in ASC.LocalAnimMontageInfo.AnimMontage,
//            only in the section the cast plays and the sections it chains into (#81)
//   wind-up = first hit notify time - Montage_GetPosition, / Montage_GetPlayRate, re-read each tick until it fires
//   landed = new hit records (LastTakeHitInfo, core's game-thread sampling) on non-players instigated by the hero (#81:
//            a listener on OnProjectileHit counted 0 in game; likely called inside the BP VM, which skips ProcessEvent)
// Game thread (game tick → montage.cpp) publishes the montage; the render thread counts records in its plain sample copy.

namespace {
    using namespace cast_indicator;

    std::atomic<bool> g_on{false};
    bool g_tracing = false;  // render thread: OnTrace subscribed
    double g_lastTick = 0;

    void OnTrace(void* objp, void* fnp, void*) {  // every ProcessEvent, only while the dev trace is armed or running
        if (g_on.load(std::memory_order_relaxed) && game::OnGameThread() && cast_trace::Active()) { cast_trace::Event(objp, fnp); cast_trace::Tick(); }
    }

    void OnEvent(void*, void*, void*) {  // game tick
        if (!g_on.load(std::memory_order_relaxed) || !game::OnGameThread()) return;
        const double now = GetTickCount64() / 1000.0;
        if (now - g_lastTick < 0.015) return;  // ~60 Hz: montage start/end checks are O(1)
        g_lastTick = now;
        cast_montage::Track();
    }

    struct CastIndicator : feature::Feature {
        Pips pips;
        Ring ring;
        Tracker tr;
        Records rec;
        std::string ability, montage, last;  // the tracked cast's names; last log line (menu)
        float windup = 0;
        int landed = 0;
        double nextTrigger = 0;
        std::string trigger;

        CastIndicator() : Feature("Cast indicator", feature::Stage::Alpha) {
            optIn = true;       // new game-thread hook
            usesCombat = true;  // hit records
        }

        void Log() {
            char b[48];
            std::snprintf(b, sizeof b, ", windup %.2fs", windup);
            last = ability + " montage " + montage + " hits " + std::to_string(tr.c.hits) + ", fired " + std::to_string(tr.c.fired) +
                   ", landed " + std::to_string(tr.c.landed) + b;
            logger::log("[cast-indicator] " + last);
        }

        // Render thread, plain data only: the published montage + core's sample copy.
        void Count(const Montage& m, const feature::Frame& f) {
            if (m.cast != tr.c.cast) {
                if (tr.Finish()) Log();  // recast before the last one settled
                tr.Begin(m.cast, m.hits);
                ability = m.ability; montage = m.name; windup = m.windup;
            }
            if (m.cast == tr.c.cast) tr.Fire(m.fired);
            if (m.ended) tr.End(f.now);
            std::vector<const combat::Sample*> fresh;
            const bool trace = cast_trace::Active();
            const int n = rec.New(f.chars, m.hero, trace ? &fresh : nullptr);
            for (const combat::Sample* s : fresh) {
                char b[160];
                std::snprintf(b, sizeof b, "record target=%llx player=%d by=%s dmg=%.1f type=%llx", (unsigned long long)s->id,
                              int(s->isPlayer), s->hitBy == m.hero ? "hero" : s->hitBy ? "other" : "none", s->hitDamage,
                              (unsigned long long)s->hitType);
                cast_trace::Note(b);
            }
            for (int i = 0; i < n; i++) {
                tr.Hit();
                landed++;
            }
            if (tr.Tick(f.now)) Log();
        }

        void OnFrame(const feature::Frame& f) override {
            if (!g_on) { g_on = true; game::OnGameTick(&OnEvent, true); }
            if (cast_trace::Wanted() != g_tracing) game::OnEvery(&OnTrace, g_tracing = !g_tracing);
            if (f.now >= nextTrigger) {  // dev trace trigger, once a second
                nextTrigger = f.now + 1.0;
                if (trigger.empty()) {
                    char exe[MAX_PATH] = {};
                    GetModuleFileNameA(nullptr, exe, MAX_PATH);
                    trigger = std::string(exe).substr(0, std::string(exe).find_last_of("\\/") + 1) + "cast-indicator.trace";
                }
                if (GetFileAttributesA(trigger.c_str()) != INVALID_FILE_ATTRIBUTES) { DeleteFileA(trigger.c_str()); cast_trace::Arm(); }
            }
            const Montage m = cast_montage::Published();
            Count(m, f);
            pips.Update(tr.c, f.now);
            ring.Update(m.cast, m.windup, f.now + (m.fireAt - cast_montage::Steady()), m.held, m.fired > 0, m.ended, f.now);  // steady → render clock
            ImDrawList* dl = ImGui::GetForegroundDrawList();  // Layer::Hud
            if (pips.Visible(f.now)) Draw(dl, f);
            if (ring.Visible(f.now)) DrawRing(dl, f);
        }

        // Spec: design-system.md § Wind-up ring. Centred on the first pip (row centre for a bar or no pips).
        void DrawRing(ImDrawList* dl, const feature::Frame& f) const {
            using namespace style;
            const float ui = type::Ui(f.h), alpha = ring.Alpha(f.now);
            const bool onPip = pips.seen == ring.cast && pips.hits > 0 && pips.hits < kBarFrom;
            const ImVec2 c{f.w * 0.5f + (onPip ? PipX(0, pips.hits, 0, ui) * pips.RowScale(f.now) : 0), f.h * hud::kPipsY};
            const float w = stroke::kBarEdge * ui;
            const Rgba tone = ring.Cancelled() ? color::kTextMuted : color::kTextSoft;
            const float rt = kRingR * ui * ring.Punch(f.now);
            if (ring.Armed(f.now)) {  // met, waiting for the shot: one bright ring with a glow, no motion
                dl->AddCircle(c, rt, Pack(color::kText, stroke::kGlowAlpha * alpha), 0, w * stroke::kGlowWidth);
                dl->AddCircle(c, rt, Pack(color::kInk, stroke::kOutlineAlpha * alpha), 0, w + 2);
                dl->AddCircle(c, rt, Pack(color::kText, alpha), 0, w);
                return;
            }
            dl->AddCircle(c, rt, Pack(color::kInk, stroke::kOutlineAlpha * alpha), 0, w + 2);
            dl->AddCircle(c, rt, Pack(Mix(tone, color::kText, ring.Flash(f.now)), alpha), 0, w * 0.5f + 0.5f);
            if (ring.Fired() || ring.Cancelled()) return;
            const float ra = ring.Approach(f.now) * ui;
            dl->AddCircle(c, ra, Pack(tone, stroke::kGlowAlpha * alpha), 0, w * stroke::kGlowWidth);
            dl->AddCircle(c, ra, Pack(color::kInk, stroke::kOutlineAlpha * alpha), 0, w + 2);
            dl->AddCircle(c, ra, Pack(tone, alpha), 0, w);
        }

        void Draw(ImDrawList* dl, const feature::Frame& f) const {
            using namespace style;
            const float ui = type::Ui(f.h), alpha = pips.Alpha(f.now), s = pips.RowScale(f.now);
            const ImVec2 c{f.w * 0.5f, f.h * hud::kPipsY};
            const Rgba fill = element::Of(combat::Element::Physical);  // ponytail: Physical only; ability element when #4 names it
            if (pips.hits >= kBarFrom) {
                const float w = hud::kBarW * ui * s, h = hud::kBarH * ui * s, x0 = c.x - w * 0.5f, y0 = c.y - h * 0.5f, seg = w / pips.hits;
                dl->AddRectFilled({x0, y0}, {x0 + w, y0 + h}, Pack(color::kInk, .55f * alpha), radius::Pill(h));
                for (int i = 0; i < pips.hits; i++) {
                    const ImVec2 a{x0 + seg * i, y0}, b{x0 + seg * (i + 1), y0 + h};
                    if (pips.Filled(i)) dl->AddRectFilled(a, b, Pack(Mix(fill, color::kText, pips.Flash(i, f.now)), alpha));
                    else if (pips.Fired(i)) dl->AddRectFilled(a, b, Pack(fill, kFiredAlpha * alpha));
                    else if (pips.Muted(i)) dl->AddRectFilled(a, b, Pack(color::kTextMuted, .55f * alpha));
                }
                for (int i = 1; i < pips.hits; i++)
                    dl->AddLine({x0 + seg * i, y0}, {x0 + seg * i, y0 + h}, Pack(color::kInk, alpha), 1);
                dl->AddRect({x0, y0}, {x0 + w, y0 + h}, Pack(color::kTextSoft, .6f * alpha), radius::Pill(h), 0, 1);
                return;
            }
            for (int i = 0; i < pips.hits; i++) {
                const ImVec2 p{c.x + PipX(i, pips.hits, 0, ui) * s, c.y};
                const float r = kPipHalf * ui * s;  // all pips equal: no deterministic per-hit damage source (#85)
                if (pips.Filled(i)) {
                    const float k = r * pips.PipScale(i, f.now);
                    dl->AddCircleFilled(p, kGlowR * k, Pack(fill, stroke::kGlowAlpha * alpha));
                    dl->AddQuadFilled({p.x, p.y - k}, {p.x + k, p.y}, {p.x, p.y + k}, {p.x - k, p.y},
                                      Pack(Mix(fill, color::kText, pips.Flash(i, f.now)), alpha));
                } else if (pips.Fired(i)) {  // shot out, no hit confirmed: hollow in the hit colour
                    const float k = r * pips.PipScale(i, f.now);
                    dl->AddQuadFilled({p.x, p.y - k}, {p.x + k, p.y}, {p.x, p.y + k}, {p.x - k, p.y}, Pack(fill, kFiredAlpha * alpha));
                    dl->AddQuad({p.x, p.y - k}, {p.x + k, p.y}, {p.x, p.y + k}, {p.x - k, p.y}, Pack(color::kInk, stroke::kOutlineAlpha * alpha), 3);
                    dl->AddQuad({p.x, p.y - k}, {p.x + k, p.y}, {p.x, p.y + k}, {p.x - k, p.y}, Pack(fill, alpha), 1.5f);
                } else {
                    const Rgba edge = pips.Muted(i) ? color::kTextMuted : color::kTextSoft;
                    dl->AddQuadFilled({p.x, p.y - r}, {p.x + r, p.y}, {p.x, p.y + r}, {p.x - r, p.y},
                                      Pack(pips.Muted(i) ? color::kTextMuted : color::kInk, .55f * alpha));
                    dl->AddQuad({p.x, p.y - r}, {p.x + r, p.y}, {p.x, p.y + r}, {p.x - r, p.y}, Pack(edge, .6f * alpha), 1);
                }
            }
        }

        // The callbacks have drained when OnGameTick/OnEvery(false) return: the game-thread state is ours again.
        void Off() override {
            g_on = false;
            game::OnGameTick(&OnEvent, false);
            if (g_tracing) game::OnEvery(&OnTrace, g_tracing = false);
            cast_montage::Reset();
            pips = {};
            ring = {};
            tr = {};
            rec = {};
        }

        void Menu() override {
            ImGui::TextDisabled("casts %d  landed hits %d", cast_montage::Casts(), landed);
            if (!last.empty()) ImGui::TextDisabled("%s", last.c_str());
        }
    } g_cast_indicator;
}

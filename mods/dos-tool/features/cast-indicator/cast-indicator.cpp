#include "feature.hpp"
#include "cast-indicator.hpp"
#include "logger.hpp"
#include "ref.hpp"
#include "style.hpp"
#include "trace.hpp"
#include "umg.hpp"
#include "imgui.h"

#include <Windows.h>
#include <atomic>
#include <chrono>
#include <unordered_set>
#include <cstdint>
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include "Engine_classes.hpp"
#include "Archon_classes.hpp"
#include "GameplayAbilities_classes.hpp"
#include "BP_GameplayAnimNotify_classes.hpp"

// Cast indicator (#3): while the local hero plays an ability montage with 2+ hit notifies, a row of pips under
// the character, one per hit the montage should land, filling per landed hit. A cast whose first hit comes after a
// wind-up gets a ring shrinking onto the first pip that meets it when the hit fires (#92).
//   N      = ApplyEffect/ShootProjectile notifies (UBP_GameplayAnimNotify_C) in ASC.LocalAnimMontageInfo.AnimMontage,
//            only in the section the cast plays and the sections it chains into (#81)
//   wind-up = first hit notify time - Montage_GetPosition, / Montage_GetPlayRate, re-read each tick until it fires
//   landed = new hit records (LastTakeHitInfo, core's game-thread sampling) on non-players instigated by the hero (#81:
//            a listener on OnProjectileHit counted 0 in game; likely called inside the BP VM, which skips ProcessEvent)
// Game thread (ProcessEvent listener) publishes the montage; the render thread counts records in its plain sample copy.
using namespace SDK;

namespace {
    using umg::PtrOk;
    using namespace cast_indicator;

    std::atomic<bool> g_on{false};
    std::atomic<int> g_casts{0};

    // Game thread state.
    ref::Cached<UClass> g_notifyCls{[] { return UBP_GameplayAnimNotify_C::StaticClass(); }};
    ref::Ref g_lastAbility, g_lastMontage;  // the ASC's montage info last seen
    bool g_lastBit = false;
    Montage g_mon;
    double g_lastTick = 0;
    std::vector<float> g_hits;  // the tracked cast's hit times (montage s)
    bool g_aiming = false;      // wind-up ring: first hit still ahead
    std::unordered_set<std::string> g_dumped;  // montages whose notifies were logged

    // Game thread -> render thread.
    SRWLOCK g_mu = SRWLOCK_INIT;
    Montage g_pub;

    void Publish() {
        AcquireSRWLockExclusive(&g_mu);
        g_pub = g_mon;
        ReleaseSRWLockExclusive(&g_mu);
    }

    double Steady() { return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count(); }

    UAnimInstance* Anim(AArchonCharacter* hero) {
        return PtrOk(hero->Mesh) && PtrOk(hero->Mesh->AnimScriptInstance) ? hero->Mesh->AnimScriptInstance : nullptr;
    }

    AArchonCharacter* Hero() {
        APlayerController* pc = umg::LocalPC();
        if (!pc || !PtrOk(pc->Pawn) || !pc->Pawn->IsA(AArchonCharacter::StaticClass())) return nullptr;
        return static_cast<AArchonCharacter*>(pc->Pawn);
    }

    std::uint64_t Id(const FName& n) { std::uint64_t v = 0; std::memcpy(&v, &n, std::min(sizeof v, sizeof n)); return v; }

    // Hit times of this cast: hit notifies of the section the montage starts in and the sections it chains into
    // (AimedShot cycles one section per cast). O(notifies + sections²) once per montage start. Game thread: calls
    // UAnimInstance::Montage_GetCurrentSection.
    std::vector<float> HitList(AArchonCharacter* hero, UAnimMontage* m) {
        UClass* cls = g_notifyCls.Get();
        if (!cls) return {};
        std::vector<Notify> ns;
        const bool dump = g_dumped.insert(m->GetName()).second;  // evidence for #81/#85: once per montage per load
        std::string line = "[cast-indicator] notifies " + m->GetName() + " (time type ApplyEffectID):";
        for (int i = 0; i < m->Notifies.Num(); i++) {
            const FAnimNotifyEvent& e = m->Notifies[i];
            UAnimNotify* a = e.Notify;
            const auto* g = PtrOk(a) && a->IsA(cls) ? static_cast<UBP_GameplayAnimNotify_C*>(a) : nullptr;
            const float t = LinkTime(int(e.LinkMethod), e.SegmentBeginTime, e.SegmentLength, e.LinkValue) + e.TriggerTimeOffset;
            ns.push_back({t, g && IsHit(int(g->mGameplayAnimNotifyType))});
            if (dump && g) {
                char b[48];
                std::snprintf(b, sizeof b, " %.2f/%d/%d", t, int(g->mGameplayAnimNotifyType), int(g->ApplyEffectID));
                line += b;
            }
        }
        if (dump) logger::log(line);
        std::vector<Section> secs;
        for (int i = 0; i < m->CompositeSections.Num(); i++) {
            const FCompositeSection& c = m->CompositeSections[i];
            secs.push_back({Id(c.SectionName), Id(c.NextSectionName), LinkTime(int(c.LinkMethod), c.SegmentBeginTime, c.SegmentLength, c.LinkValue)});
        }
        int start = -1;
        if (UAnimInstance* ai = Anim(hero)) {
            const std::uint64_t cur = Id(ai->Montage_GetCurrentSection(m));
            for (int i = 0; cur && i < int(secs.size()); i++) if (secs[i].name == cur) start = i;
        }
        return HitTimes(secs, ns, m->SequenceLength, start);
    }

    // Seconds until the tracked cast's first hit (live position and play rate = attack speed); < 0 = fired or unknown.
    float FireIn(AArchonCharacter* hero, UAnimMontage* m) {
        UAnimInstance* ai = Anim(hero);
        return ai ? cast_indicator::FireIn(g_hits, ai->Montage_GetPosition(m), ai->Montage_GetPlayRate(m)) : -1;
    }

    // The hero's montage info changed: the old cast's montage ended, maybe a new one starts.
    void Track() {
        AArchonCharacter* hero = Hero();
        UAbilitySystemComponent* asc = hero ? hero->mAbilitySystemComponent : nullptr;
        if (!PtrOk(asc)) return;
        const FGameplayAbilityLocalAnimMontage& li = asc->LocalAnimMontageInfo;
        UGameplayAbility* ab = PtrOk(li.AnimatingAbility) ? li.AnimatingAbility : nullptr;
        UAnimMontage* m = PtrOk(li.AnimMontage) && li.AnimMontage->IsA(UAnimMontage::StaticClass()) ? li.AnimMontage : nullptr;
        if (!ab) m = nullptr;  // AnimatingAbility is cleared when the montage ends; AnimMontage may stay
        auto same = [](const ref::Ref& r, const void* o) { return o ? r.Is(o) : !r.ptr; };
        if (same(g_lastAbility, ab) && same(g_lastMontage, m) && g_lastBit == li.PlayBit) {
            if (!g_aiming || !m) return;
            const float in = FireIn(hero, m);  // O(1) per tick while the wind-up runs
            g_aiming = in >= 0;
            if (g_aiming) { g_mon.fireAt = Steady() + in; Publish(); }
            return;
        }
        g_lastAbility = ref::Ref(ab);
        g_lastMontage = ref::Ref(m);
        g_lastBit = li.PlayBit;

        g_hits = m ? HitList(hero, m) : std::vector<float>{};
        const float in = m ? FireIn(hero, m) : -1;
        const float windup = std::max(in, 0.0f);
        const int pips = m ? PipsFor(int(g_hits.size()), windup, Spread(ab->Class->GetName())) : 0;
        g_aiming = windup >= kWindupMin;
        if (pips > 0 || g_aiming) {
            g_mon = {g_mon.cast + 1, pips, false, reinterpret_cast<std::uintptr_t>(hero), ab->Class->GetName(), m->GetName(),
                     windup, Steady() + windup};
            g_casts++;
            cast_trace::Begin((g_mon.ability + " montage " + g_mon.name + " hits " + std::to_string(g_hits.size())).c_str());
        } else {
            g_mon.ended = true;  // render thread: arrows in flight still count for kLateHits
            g_aiming = false;
            cast_trace::End();
        }
        Publish();
    }

    void OnEvent(void* objp, void* fnp, void*) {
        if (!g_on.load(std::memory_order_relaxed) || !game::OnGameThread()) return;
        if (cast_trace::Active()) { cast_trace::Event(objp, fnp); cast_trace::Tick(); }
        const double now = GetTickCount64() / 1000.0;
        if (now - g_lastTick < 0.015) return;  // ~60 Hz: montage start/end checks are O(1)
        g_lastTick = now;
        Track();
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
            last = ability + " montage " + montage + " hits " + std::to_string(tr.c.hits) + ", landed " + std::to_string(tr.c.landed) + b;
            logger::log("[cast-indicator] " + last);
        }

        // Render thread, plain data only: the published montage + core's sample copy.
        void Count(const Montage& m, const feature::Frame& f) {
            if (m.cast != tr.c.cast) {
                if (tr.Finish()) Log();  // recast before the last one settled
                tr.Begin(m.cast, m.hits);
                ability = m.ability; montage = m.name; windup = m.windup;
            }
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
            if (!g_on) { g_on = true; game::SetEventListener(&OnEvent, true); }
            if (f.now >= nextTrigger) {  // dev trace trigger, once a second
                nextTrigger = f.now + 1.0;
                if (trigger.empty()) {
                    char exe[MAX_PATH] = {};
                    GetModuleFileNameA(nullptr, exe, MAX_PATH);
                    trigger = std::string(exe).substr(0, std::string(exe).find_last_of("\\/") + 1) + "cast-indicator.trace";
                }
                if (GetFileAttributesA(trigger.c_str()) != INVALID_FILE_ATTRIBUTES) { DeleteFileA(trigger.c_str()); cast_trace::Arm(); }
            }
            AcquireSRWLockShared(&g_mu);
            const Montage m = g_pub;
            ReleaseSRWLockShared(&g_mu);
            Count(m, f);
            pips.Update(tr.c, f.now);
            ring.Update(m.cast, m.windup, f.now + (m.fireAt - Steady()), m.ended, f.now);  // steady → render clock
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
            dl->AddCircle(c, rt, Pack(color::kInk, stroke::kOutlineAlpha * alpha), 0, w + 2);
            dl->AddCircle(c, rt, Pack(Mix(tone, color::kText, ring.Flash(f.now)), alpha), 0, w * 0.5f + 0.5f);
            if (ring.Fired(f.now) || ring.Cancelled()) return;
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
                } else {
                    const Rgba edge = pips.Muted(i) ? color::kTextMuted : color::kTextSoft;
                    dl->AddQuadFilled({p.x, p.y - r}, {p.x + r, p.y}, {p.x, p.y + r}, {p.x - r, p.y},
                                      Pack(pips.Muted(i) ? color::kTextMuted : color::kInk, .55f * alpha));
                    dl->AddQuad({p.x, p.y - r}, {p.x + r, p.y}, {p.x, p.y + r}, {p.x - r, p.y}, Pack(edge, .6f * alpha), 1);
                }
            }
        }

        // The listener has drained when SetEventListener returns: the game-thread state is ours again.
        void Off() override {
            g_on = false;
            game::SetEventListener(&OnEvent, false);
            g_lastAbility = g_lastMontage = {};
            g_mon = g_pub = {};
            g_hits.clear();
            g_aiming = false;
            g_dumped.clear();
            pips = {};
            ring = {};
            tr = {};
            rec = {};
        }

        void Menu() override {
            ImGui::TextDisabled("casts %d  landed hits %d", g_casts.load(), landed);
            if (!last.empty()) ImGui::TextDisabled("%s", last.c_str());
        }
    } g_cast_indicator;
}

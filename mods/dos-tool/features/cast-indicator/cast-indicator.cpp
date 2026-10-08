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
#include <cstdint>
#include <string>
#include "Engine_classes.hpp"
#include "Archon_classes.hpp"
#include "Archon_parameters.hpp"
#include "GameplayAbilities_classes.hpp"
#include "BP_GameplayAnimNotify_classes.hpp"

// Cast indicator (#3): while the local hero plays an ability montage with 2+ hit notifies, a row of pips under
// the character, one per hit the montage should land, filling per landed hit.
//   N      = ApplyEffect/ShootProjectile notifies (UBP_GameplayAnimNotify_C) in ASC.LocalAnimMontageInfo.AnimMontage
//   landed = OnProjectileHit(Hit, Instigator) on that montage's ability with Hit.Actor a character other than the hero
// Game thread (ProcessEvent listener) reads the game and publishes plain counts; the render thread only animates them.
using namespace SDK;

namespace {
    using umg::PtrOk;
    using namespace cast_indicator;

    std::atomic<bool> g_on{false};
    std::atomic<int> g_casts{0}, g_projectileHits{0}, g_landed{0};

    // Game thread state.
    ref::Fn g_fnHit{UArchonGameplayAbility::StaticClass, "ArchonGameplayAbility", "OnProjectileHit"};
    ref::Cached<UClass> g_notifyCls{[] { return UBP_GameplayAnimNotify_C::StaticClass(); }};
    int32 g_hitName = -1;  // FName index of OnProjectileHit: Blueprint overrides are other UFunctions with this name
    ref::Ref g_lastAbility, g_lastMontage;  // the ASC's montage info last seen
    bool g_lastBit = false;
    ref::Ref g_castAbility;                 // the shown cast's ability: its hits count until the cast is done
    std::string g_castName, g_montageName;
    Tracker g_tr;
    double g_lastTick = 0;

    // Game thread → render thread.
    SRWLOCK g_mu = SRWLOCK_INIT;
    Cast g_pub;
    std::string g_lastLine;  // menu readout

    void Publish() {
        AcquireSRWLockExclusive(&g_mu);
        g_pub = g_tr.c;
        ReleaseSRWLockExclusive(&g_mu);
    }

    void Done() {
        const std::string line = "[cast-indicator] " + g_castName + " montage " + g_montageName + " hits "
                               + std::to_string(g_tr.c.hits) + ", landed " + std::to_string(g_tr.c.landed);
        logger::log(line);
        AcquireSRWLockExclusive(&g_mu);
        g_lastLine = line;
        ReleaseSRWLockExclusive(&g_mu);
        g_castAbility = {};
    }

    AArchonCharacter* Hero() {
        APlayerController* pc = umg::LocalPC();
        if (!pc || !PtrOk(pc->Pawn) || !pc->Pawn->IsA(AArchonCharacter::StaticClass())) return nullptr;
        return static_cast<AArchonCharacter*>(pc->Pawn);
    }

    // O(notifies in the montage), once per montage start.
    int CountHits(const UAnimMontage* m) {
        UClass* cls = g_notifyCls.Get();
        if (!cls) return 0;
        int n = 0;
        const auto& ns = m->Notifies;
        for (int i = 0; i < ns.Num(); i++) {
            UAnimNotify* a = ns[i].Notify;
            if (PtrOk(a) && a->IsA(cls) && IsHit(int(static_cast<UBP_GameplayAnimNotify_C*>(a)->mGameplayAnimNotifyType))) n++;
        }
        return n;
    }

    // The hero's montage info changed: the old cast's montage ended, maybe a new one starts.
    void Track(double now) {
        AArchonCharacter* hero = Hero();
        UAbilitySystemComponent* asc = hero ? hero->mAbilitySystemComponent : nullptr;
        if (!PtrOk(asc)) return;
        const FGameplayAbilityLocalAnimMontage& li = asc->LocalAnimMontageInfo;
        UGameplayAbility* ab = PtrOk(li.AnimatingAbility) ? li.AnimatingAbility : nullptr;
        UAnimMontage* m = PtrOk(li.AnimMontage) && li.AnimMontage->IsA(UAnimMontage::StaticClass()) ? li.AnimMontage : nullptr;
        if (!ab) m = nullptr;  // AnimatingAbility is cleared when the montage ends; AnimMontage may stay
        auto same = [](const ref::Ref& r, const void* o) { return o ? r.Is(o) : !r.ptr; };
        if (same(g_lastAbility, ab) && same(g_lastMontage, m) && g_lastBit == li.PlayBit) return;
        g_lastAbility = ref::Ref(ab);
        g_lastMontage = ref::Ref(m);
        g_lastBit = li.PlayBit;

        const int hits = m ? CountHits(m) : 0;
        if (hits >= kMinHits) {
            if (g_tr.Finish()) Done();  // recast before the last one settled
            g_tr.Begin(hits);
            g_castAbility = ref::Ref(ab);
            g_castName = ab->Class->GetName();
            g_montageName = m->GetName();
            g_casts++;
            cast_trace::Begin((g_castName + " montage " + g_montageName + " hits " + std::to_string(hits)).c_str());
        } else {
            g_tr.End(now);  // arrows in flight still count for kLateHits
            cast_trace::End();
        }
        Publish();
    }

    // Hit.Actor (weak) is a character other than the hero: the arrow landed on someone, not on a wall.
    bool Landed(const void* parms) {
        if (!parms) return false;
        const auto* p = static_cast<const Params::ArchonGameplayAbility_OnProjectileHit*>(parms);
        UObject* target = p->Hit.Actor.Get();
        return PtrOk(target) && target->IsA(AArchonCharacter::StaticClass()) && target != Hero();
    }

    void OnEvent(void* objp, void* fnp, void* parms) {
        if (!g_on.load(std::memory_order_relaxed) || !game::OnGameThread()) return;
        if (cast_trace::Active()) { cast_trace::Event(objp, fnp); cast_trace::Tick(); }
        if (g_hitName < 0) {
            UFunction* f = g_fnHit.Get();
            if (!f) return;
            g_hitName = f->Name.ComparisonIndex;  // native class: the index outlives map travel
        }
        const double now = GetTickCount64() / 1000.0;
        if (PtrOk(fnp) && static_cast<const UFunction*>(fnp)->Name.ComparisonIndex == g_hitName) {
            g_projectileHits++;
            if (!g_castAbility.Is(objp) || !Landed(parms)) return;
            g_landed++;
            g_tr.Hit();
            if (g_tr.Tick(now)) Done();
            Publish();
            return;
        }
        if (now - g_lastTick < 0.015) return;  // ~60 Hz: montage start/end checks are O(1)
        g_lastTick = now;
        Track(now);
        if (g_tr.Tick(now)) { Done(); Publish(); }
    }

    struct CastIndicator : feature::Feature {
        Pips pips;
        double nextTrigger = 0;
        std::string trigger;

        CastIndicator() : Feature("Cast indicator", feature::Stage::Alpha) { optIn = true; }  // new game-thread hook

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
            const Cast c = g_pub;
            ReleaseSRWLockShared(&g_mu);
            pips.Update(c, f.now);
            if (!pips.Visible(f.now)) return;
            Draw(ImGui::GetForegroundDrawList(), f);  // Layer::Hud
        }

        void Draw(ImDrawList* dl, const feature::Frame& f) const {
            using namespace style;
            const float ui = type::Ui(f.h), alpha = pips.Alpha(f.now), s = pips.RowScale(f.now);
            const ImVec2 c{f.w * 0.5f, f.h * kAnchorY};
            const Rgba fill = element::Of(combat::Element::Physical);  // ponytail: Physical only; ability element when #4 names it
            if (pips.hits >= kBarFrom) {
                const float w = kBarW * ui * s, h = kBarH * ui * s, x0 = c.x - w * 0.5f, y0 = c.y - h * 0.5f, seg = w / pips.hits;
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
                const float r = kPipHalf * ui * s;
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
            g_tr = {};
            g_castAbility = g_lastAbility = g_lastMontage = {};
            g_pub = {};
            pips = {};
        }

        void Menu() override {
            AcquireSRWLockShared(&g_mu);
            const std::string last = g_lastLine;
            ReleaseSRWLockShared(&g_mu);
            ImGui::TextDisabled("casts %d  projectile hits %d  landed %d", g_casts.load(), g_projectileHits.load(), g_landed.load());
            if (!last.empty()) ImGui::TextDisabled("%s", last.c_str() + sizeof("[cast-indicator]"));
        }
    } g_cast_indicator;
}

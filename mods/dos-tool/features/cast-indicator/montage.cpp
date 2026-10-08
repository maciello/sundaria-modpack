#include "montage.hpp"
#include "logger.hpp"
#include "ref.hpp"
#include "trace.hpp"
#include "umg.hpp"

#include <Windows.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <unordered_set>
#include <vector>
#include "Engine_classes.hpp"
#include "Archon_classes.hpp"
#include "GameplayAbilities_classes.hpp"
#include "BP_GameplayAnimNotify_classes.hpp"
#include "BP_GameAbility_ShootArrow_classes.hpp"

// Cast indicator, game thread: reads the hero's playing montage (sections, hit notifies; then each tick its section,
// position, play rate) and publishes a plain Montage for the render thread. Called from cast-indicator.cpp's listener.
// Model: timeline.hpp. Per tick O(notifies of the montage).
using namespace SDK;

namespace {
    using umg::PtrOk;
    using namespace cast_indicator;

    std::atomic<int> g_casts{0};

    // Game thread state.
    ref::Cached<UClass> g_notifyCls{[] { return UBP_GameplayAnimNotify_C::StaticClass(); }};
    ref::Ref g_lastAbility, g_lastMontage;  // the ASC's montage info last seen
    bool g_lastBit = false;
    Montage g_mon;
    Timeline g_tl;              // the tracked cast's montage
    int g_release = -1;         // hold abilities: the section the release jumps to (ReleaseAnimInfo)
    bool g_live = false;        // a shown cast is playing: follow it each tick
    int g_lastSec = -2;         // section and position at the last tick (fired = notifies crossed since)
    float g_lastPos = 0;
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

    // The montage as plain data + the section the cast starts in. O(notifies + sections) once per montage start.
    int Load(AArchonCharacter* hero, UGameplayAbility* ab, UAnimMontage* m) {
        g_tl = {};
        g_tl.length = m->SequenceLength;
        g_release = -1;
        UClass* cls = g_notifyCls.Get();
        if (!cls) return -1;
        const bool dump = g_dumped.insert(m->GetName()).second;  // evidence for #81/#85: once per montage per load
        std::string line = "[cast-indicator] notifies " + m->GetName() + " (time type ApplyEffectID):";
        for (int i = 0; i < m->Notifies.Num(); i++) {
            const FAnimNotifyEvent& e = m->Notifies[i];
            UAnimNotify* a = e.Notify;
            const auto* g = PtrOk(a) && a->IsA(cls) ? static_cast<UBP_GameplayAnimNotify_C*>(a) : nullptr;
            const float t = LinkTime(int(e.LinkMethod), e.SegmentBeginTime, e.SegmentLength, e.LinkValue) + e.TriggerTimeOffset;
            g_tl.ns.push_back({t, g && IsHit(int(g->mGameplayAnimNotifyType))});
            if (dump && g) {
                char b[48];
                std::snprintf(b, sizeof b, " %.2f/%d/%d", t, int(g->mGameplayAnimNotifyType), int(g->ApplyEffectID));
                line += b;
            }
        }
        if (dump) logger::log(line);
        for (int i = 0; i < m->CompositeSections.Num(); i++) {
            const FCompositeSection& c = m->CompositeSections[i];
            g_tl.secs.push_back({Id(c.SectionName), Id(c.NextSectionName), LinkTime(int(c.LinkMethod), c.SegmentBeginTime, c.SegmentLength, c.LinkValue)});
        }
        if (ab->IsA(UBP_GameAbility_ShootArrow_C::StaticClass())) {
            const auto* sa = static_cast<const UBP_GameAbility_ShootArrow_C*>(ab);
            if (sa->CanHold) g_release = g_tl.Find(Id(sa->ReleaseAnimInfo.mSectionName));
        }
        UAnimInstance* ai = Anim(hero);
        return ai ? g_tl.Find(Id(ai->Montage_GetCurrentSection(m))) : -1;
    }

    // Each tick while the cast plays: hit notifies passed (fired) and when the next one fires (live play rate).
    void Follow(AArchonCharacter* hero, UAnimMontage* m) {
        UAnimInstance* ai = Anim(hero);
        if (!ai) return;
        const int cur = g_tl.Find(Id(ai->Montage_GetCurrentSection(m)));
        const float pos = ai->Montage_GetPosition(m);
        const float from = cur != g_lastSec || pos < g_lastPos ? g_tl.Begin(cur) - 1e-3f : g_lastPos;
        g_mon.fired += g_tl.Crossed(cur, from, pos);
        g_lastSec = cur;
        g_lastPos = pos;
        const Timeline::Aim a = g_tl.Ahead(cur, pos, ai->Montage_GetPlayRate(m), g_release);
        g_mon.held = a.held;
        if (a.in >= 0) g_mon.fireAt = Steady() + a.in;
        Publish();
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
            if (g_live && m) Follow(hero, m);
            return;
        }
        // The cast's ability ends in the frame its arrow spawns (ShootArrow: SpawnProjectile → EndAbility), often before a
        // tick sees the notify pass: SpawnedArrow (reset per activation) says it fired.
        if (g_live && !g_mon.fired)
            if (const auto* sa = g_lastAbility.Get<UBP_GameAbility_ShootArrow_C>(); sa && sa->IsA(UBP_GameAbility_ShootArrow_C::StaticClass()) && sa->SpawnedArrow)
                g_mon.fired = 1;
        g_lastAbility = ref::Ref(ab);
        g_lastMontage = ref::Ref(m);
        g_lastBit = li.PlayBit;
        g_live = false;

        const int start = m ? Load(hero, ab, m) : -1;
        const int hits = m ? int(g_tl.Hits(start, g_release).size()) : 0;
        UAnimInstance* ai = m ? Anim(hero) : nullptr;
        const float in = ai ? g_tl.Ahead(start, ai->Montage_GetPosition(m), ai->Montage_GetPlayRate(m), g_release).in : -1;
        const float windup = std::max(in, 0.0f);
        const int pips = m ? PipsFor(hits, windup, Spread(ab->Class->GetName())) : 0;
        if (pips > 0 || windup >= kWindupMin) {
            g_mon = {g_mon.cast + 1, pips, false, reinterpret_cast<std::uintptr_t>(hero), ab->Class->GetName(), m->GetName(),
                     windup, Steady() + windup, false, 0};
            g_casts++;
            g_live = true;
            g_lastSec = -2;
            cast_trace::Begin((g_mon.ability + " montage " + g_mon.name + " hits " + std::to_string(hits)).c_str());
            Follow(hero, m);  // notifies already passed (RapidShot's first arrow at 0.01 s)
            return;
        }
        g_mon.ended = true;  // render thread: arrows in flight still count for kLateHits
        cast_trace::End();
        Publish();
    }
}

namespace cast_montage {
    void Track() { ::Track(); }
    double Steady() { return ::Steady(); }
    int Casts() { return g_casts.load(); }
    Montage Published() {
        AcquireSRWLockShared(&g_mu);
        const Montage m = g_pub;
        ReleaseSRWLockShared(&g_mu);
        return m;
    }
    void Reset() {
        g_lastAbility = g_lastMontage = {};
        g_mon = g_pub = {};
        g_tl = {};
        g_release = -1;
        g_live = false;
        g_dumped.clear();
    }
}

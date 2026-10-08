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

// Cast indicator, game thread: reads the hero's playing montage (hit notify times, section, position, play rate) and
// publishes a plain Montage for the render thread. Called from cast-indicator.cpp's ProcessEvent listener.
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
        g_hits.clear();
        g_aiming = false;
        g_dumped.clear();
    }
}

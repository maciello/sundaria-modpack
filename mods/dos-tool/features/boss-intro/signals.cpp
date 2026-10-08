#include "signals.hpp"
#include "game.hpp"
#include "ref.hpp"
#include "umg.hpp"

#include <Windows.h>
#include <algorithm>
#include <atomic>
#include "Engine_classes.hpp"
#include "UMG_classes.hpp"
#include "Archon_classes.hpp"
#include "BP_BossFight_classes.hpp"

// Boss signals = ProcessEvent calls the game makes anyway (#17). Functions are matched by FName index (an int that
// lives as long as the process, unlike the Blueprint UFunction it came from: #63), objects by IsA / class name.
using namespace SDK;
using umg::PtrOk;

namespace {
    enum Watch { kBeginPlay, kConstruct, kArena, kSpawn, kCombat, kFinished, kWatchCount };
    struct Fn { const char* cls; const char* name; std::atomic<int32> idx{-1}; };
    Fn g_fn[kWatchCount] = {
        {"Actor", "ReceiveBeginPlay"},
        {"UserWidget", "Construct"},
        {"BP_BossFight_C", "BndEvt__ArenaTrigger_K2Node_ComponentBoundEvent_1_ComponentBeginOverlapSignature__DelegateSignature"},
        {"BP_BossFight_C", "BndEvt__MasterSpawnTrigger_K2Node_ComponentBoundEvent_0_ComponentBeginOverlapSignature__DelegateSignature"},
        {"BP_BossFight_C", "MulticastNotifyCombatStart"},
        {"BP_BossFight_C", "MulticastNotifyFinished"},
    };
    std::atomic<bool> g_on{false}, g_tryClass{false};
    std::atomic<int32> g_splashCls{-1}, g_lensCls{-1};  // FName index of the class names, once seen
    SRWLOCK g_mu = SRWLOCK_INIT;
    std::vector<boss_intro::game_side::Event> g_events;
    std::vector<ref::Ref> g_fights;  // ABP_BossFight_C seen by a signal, newest last

    void Resolve(const UClass* c, int from, int to) {
        if (!PtrOk(c)) return;
        for (int i = from; i < to; i++)
            if (g_fn[i].idx.load() < 0)
                if (UFunction* f = c->GetFunction(g_fn[i].cls, g_fn[i].name); PtrOk(f)) g_fn[i].idx = f->Name.ComparisonIndex;
    }

    std::string Text(const FText& t) { return PtrOk(t.TextData) ? t.ToString() : ""; }

    void Push(boss_intro::Signal s, const UObject* fight) {
        boss_intro::game_side::Event e{s, reinterpret_cast<std::uintptr_t>(fight)};
        if (fight) {
            e.fightClass = fight->Class->GetName();
            if (fight->IsA(ABP_BossFight_C::StaticClass())) {
                auto* bf = static_cast<const ABP_BossFight_C*>(fight);
                e.name = Text(bf->FightDisplayName);
                e.subtitle = Text(bf->FightStartedMessage);
            } else {
                fight = nullptr;  // no BossActors to frame
            }
        }
        AcquireSRWLockExclusive(&g_mu);
        if (fight && std::find(g_fights.begin(), g_fights.end(), ref::Ref(fight)) == g_fights.end()) {
            if (g_fights.size() >= 8) g_fights.erase(g_fights.begin());  // a dungeon has a handful of fights
            g_fights.push_back(ref::Ref(fight));
        }
        if (g_events.size() < 64) g_events.push_back(std::move(e));
        ReleaseSRWLockExclusive(&g_mu);
    }

    // Exact class name match, remembered as an FName index (no class pointer kept).
    bool ClassNamed(const UObject* o, std::atomic<int32>& idx, const char* name) {
        const int32 have = idx.load(std::memory_order_relaxed);
        if (have >= 0) return o->Class->Name.ComparisonIndex == have;
        if (o->Class->GetName() != name) return false;
        idx = o->Class->Name.ComparisonIndex;
        return true;
    }

    bool LocalPawn(const void* parms) {  // overlap parms: OtherActor @0x08
        const APlayerController* pc = umg::LocalPC();
        const void* other = *reinterpret_cast<void* const*>(static_cast<const char*>(parms) + 0x08);
        return pc && PtrOk(pc->Pawn) && other == pc->Pawn;
    }

    // Every ProcessEvent call: one int compare per watched name until one matches.
    void OnEvent(void* objp, void* fnp, void* parms) {
        using boss_intro::Signal;
        if (!g_on.load(std::memory_order_relaxed) || !PtrOk(fnp) || !PtrOk(objp)) return;
        if (g_fn[kBeginPlay].idx.load(std::memory_order_relaxed) < 0 || g_tryClass.load(std::memory_order_relaxed)) {
            if (!game::OnGameThread()) return;
            Resolve(AActor::StaticClass(), kBeginPlay, kBeginPlay + 1);
            Resolve(UUserWidget::StaticClass(), kConstruct, kConstruct + 1);
            if (g_tryClass.exchange(false)) Resolve(ABP_BossFight_C::StaticClass(), kArena, kWatchCount);  // loaded only in a dungeon
        }
        const int32 n = static_cast<const UFunction*>(fnp)->Name.ComparisonIndex;
        int w = 0;
        while (w < kWatchCount && g_fn[w].idx.load(std::memory_order_relaxed) != n) w++;
        if (w == kWatchCount) return;
        const auto* obj = static_cast<const UObject*>(objp);
        if (!PtrOk(obj->Class)) return;
        switch (w) {
            case kBeginPlay:
                if (obj->IsA(AArchonBossFight::StaticClass())) {
                    Resolve(obj->Class, kArena, kWatchCount);  // the first fight of a map names the fight functions
                    Push(Signal::FightBegin, obj);
                } else if (obj->IsA(AEmitterCameraLensEffectBase::StaticClass()) && ClassNamed(obj, g_lensCls, "BP_LensEffect_bossAnnouncement_C")) {
                    Push(Signal::Lens, nullptr);
                }
                return;
            case kConstruct:
                if (ClassNamed(obj, g_splashCls, "WidgetBossSplashScreen_C")) Push(Signal::Splash, nullptr);
                return;
            default:
                if (!obj->IsA(AArchonBossFight::StaticClass())) return;
                if ((w == kArena || w == kSpawn) && (!PtrOk(parms) || !LocalPawn(parms))) return;
                Push(w == kArena ? Signal::ArenaEnter : w == kSpawn ? Signal::SpawnTrigger : w == kCombat ? Signal::CombatStart : Signal::Finished, obj);
        }
    }
}

namespace boss_intro::game_side {
    void Listen(bool on) {
        if (on == g_on.load()) return;
        g_on = on;
        g_tryClass = on;  // after a hot reload inside a dungeon the fights' BeginPlay is long past
        game::SetEventListener(&OnEvent, on);
    }

    std::uintptr_t LocalPawn() {
        const APlayerController* pc = umg::LocalPC();
        return pc && PtrOk(pc->Pawn) ? reinterpret_cast<std::uintptr_t>(pc->Pawn) : 0;
    }

    static ref::Ref FightRef(std::uintptr_t fight) {
        ref::Ref r;
        AcquireSRWLockShared(&g_mu);
        for (const ref::Ref& f : g_fights)
            if (reinterpret_cast<std::uintptr_t>(f.ptr) == fight) r = f;
        ReleaseSRWLockShared(&g_mu);
        return r;
    }

    bool Alive(std::uintptr_t fight) { return FightRef(fight).Get<ABP_BossFight_C>() != nullptr; }

    bool BossOf(std::uintptr_t fight, Boss& out) {
        const auto* bf = FightRef(fight).Get<ABP_BossFight_C>();
        if (!bf || bf->BossActors.Num() < 1) return false;
        const auto* boss = ref::Ref(bf->BossActors[0]).Get<ACharacter>();
        if (!boss || !boss->IsA(ACharacter::StaticClass()) || !PtrOk(boss->RootComponent) || !PtrOk(boss->CapsuleComponent)) return false;
        const FVector& p = boss->RootComponent->RelativeLocation;  // root unattached: relative == world (game-facts.md)
        const UCapsuleComponent* c = boss->CapsuleComponent;
        out = {p.X, p.Y, p.Z, c->CapsuleHalfHeight * c->RelativeScale3D.Z};
        return out.halfHeight > 1.0f;
    }

    std::vector<Event> Take() {
        std::vector<Event> out;
        AcquireSRWLockExclusive(&g_mu);
        out.swap(g_events);
        ReleaseSRWLockExclusive(&g_mu);
        return out;
    }
}

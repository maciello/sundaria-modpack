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
    enum Watch { kBeginPlay, kConstruct, kCamera, kArena, kSpawn, kCombat, kFinished, kWatchCount };
    struct Fn { const char* cls; const char* name; std::atomic<int32> idx{-1}; };
    Fn g_fn[kWatchCount] = {
        {"Actor", "ReceiveBeginPlay"},
        {"UserWidget", "Construct"},
        {"PlayerCameraManager", "BlueprintUpdateCamera"},
        {"BP_BossFight_C", "BndEvt__ArenaTrigger_K2Node_ComponentBoundEvent_1_ComponentBeginOverlapSignature__DelegateSignature"},
        {"BP_BossFight_C", "BndEvt__MasterSpawnTrigger_K2Node_ComponentBoundEvent_0_ComponentBeginOverlapSignature__DelegateSignature"},
        {"BP_BossFight_C", "MulticastNotifyCombatStart"},
        {"BP_BossFight_C", "MulticastNotifyFinished"},
    };
    std::atomic<bool> g_on{false}, g_tryClass{false};
    std::atomic<int32> g_splashCls{-1}, g_lensCls{-1};  // FName index of the class names, once seen
    SRWLOCK g_mu = SRWLOCK_INIT;
    std::vector<boss_intro::game_side::Event> g_events;
    struct SweepReq { float from[3], to[3]; std::uintptr_t fight; } g_sweep{};  // g_mu
    std::atomic<bool> g_sweepOn{false};
    std::atomic<float> g_clear{1.0f};
    std::vector<ref::Ref> g_fights;  // ABP_BossFight_C seen by a signal, newest last
    std::vector<boss_intro::pause::Held<ref::Ref>> g_held;  // game thread (GameTick), or Resume's unload fallback
    // render thread → game thread (#80): the fight to frame and to freeze, a resume request, and the answers
    std::atomic<std::uintptr_t> g_bossWant{0}, g_pauseFight{0};
    std::atomic<int> g_resumeReq{0};  // 0 none, 1 asked, 2 the game thread is resuming
    std::atomic<int> g_paused{0}, g_resumed{0};
    struct BossRead { std::uintptr_t fight; bool ok; boss_intro::game_side::Boss boss; } g_boss{};  // g_mu
    void GameTick();

    void HoldOne(AActor* a, int& n) {  // a: live (ref-checked)
        if (a && a->Role == ENetRole::ROLE_Authority && boss_intro::pause::Hold(g_held, ref::Ref(a), a->CustomTimeDilation)) n++;
    }

    ref::Ref FightRef(std::uintptr_t fight) {
        ref::Ref r;
        AcquireSRWLockShared(&g_mu);
        for (const ref::Ref& f : g_fights)
            if (reinterpret_cast<std::uintptr_t>(f.ptr) == fight) r = f;
        ReleaseSRWLockShared(&g_mu);
        return r;
    }

    // #71, game thread, inside the camera update: one sphere sweep per frame while an intro asks for it.
    void DoSweep(const UObject* cameraManager) {
        AcquireSRWLockShared(&g_mu);
        const SweepReq s = g_sweep;
        ReleaseSRWLockShared(&g_mu);
        AActor* ignore[9];
        int n = 0;
        if (const auto* bf = FightRef(s.fight).Get<ABP_BossFight_C>())
            for (const TArray<AActor*>* list : {&bf->BossActors, &bf->PartnerActors})
                for (int i = 0; i < list->Num() && n < 8; i++)
                    if (AActor* a = ref::Ref((*list)[i]).Get<AActor>()) ignore[n++] = a;
        if (const APlayerController* pc = umg::LocalPC(); pc && PtrOk(pc->Pawn)) ignore[n++] = pc->Pawn;
        const TArray<AActor*> ignored(ignore, n, n);  // non-owning view of the stack array
        FHitResult hit{};
        // TraceTypeQuery2 = the Camera channel in UE's default channel order (unverified for this game's config)
        const bool blocked = UKismetSystemLibrary::SphereTraceSingle(cameraManager, FVector{s.from[0], s.from[1], s.from[2]},
            FVector{s.to[0], s.to[1], s.to[2]}, boss_intro::cam::kProbe, ETraceTypeQuery::TraceTypeQuery2, false, ignored,
            EDrawDebugTrace::None, &hit, true, FLinearColor{}, FLinearColor{}, 0.0f);
        g_clear = blocked && !hit.bStartPenetrating ? std::clamp(hit.Time, 0.0f, 1.0f) : 1.0f;
    }

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
            Resolve(APlayerCameraManager::StaticClass(), kCamera, kCamera + 1);
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
            case kCamera:
                if (!game::OnGameThread()) return;
                GameTick();
                if (g_sweepOn.load(std::memory_order_relaxed)) DoSweep(obj);
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
    bool BossNow(std::uintptr_t fight, Boss& out);
    int ResumeNow();

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

    bool Alive(std::uintptr_t fight) { return FightRef(fight).Get<ABP_BossFight_C>() != nullptr; }

    bool BossOf(std::uintptr_t fight, Boss& out) {
        g_bossWant = fight;
        AcquireSRWLockShared(&g_mu);
        const BossRead r = g_boss;
        ReleaseSRWLockShared(&g_mu);
        if (r.fight != fight || !r.ok) return false;
        out = r.boss;
        return true;
    }

    int Pause(std::uintptr_t fight) {
        g_pauseFight = fight;
        return g_paused.exchange(0);
    }

    int Resume() {
        g_pauseFight = 0;
        g_resumeReq = 1;
        for (int i = 0; i < 50 && g_resumeReq.load(); i++) Sleep(2);
        int asked = 1;
        if (g_resumeReq.compare_exchange_strong(asked, 0)) return ResumeNow();  // no camera update came (unload): the hooks are gone
        for (int i = 0; i < 50 && g_resumeReq.load(); i++) Sleep(2);
        return g_resumed.exchange(0);
    }

    bool BossNow(std::uintptr_t fight, Boss& out) {
        const auto* bf = FightRef(fight).Get<ABP_BossFight_C>();
        if (!bf || bf->BossActors.Num() < 1) return false;
        const auto* boss = ref::Ref(bf->BossActors[0]).Get<ACharacter>();
        if (!boss || !boss->IsA(ACharacter::StaticClass()) || !PtrOk(boss->RootComponent) || !PtrOk(boss->CapsuleComponent)) return false;
        const FVector& p = boss->RootComponent->RelativeLocation;  // root unattached: relative == world (game-facts.md)
        const UCapsuleComponent* c = boss->CapsuleComponent;
        out = {p.X, p.Y, p.Z, c->CapsuleHalfHeight * c->RelativeScale3D.Z};
        return out.halfHeight > 1.0f;
    }

    int PauseNow(std::uintptr_t fight) {
        const auto* bf = FightRef(fight).Get<ABP_BossFight_C>();
        if (!bf) return 0;
        int n = 0;
        for (const TArray<AActor*>* list : {&bf->BossActors, &bf->PartnerActors})
            for (int i = 0; i < list->Num(); i++) {
                AActor* a = ref::Ref((*list)[i]).Get<AActor>();
                HoldOne(a, n);
                if (a && a->IsA(APawn::StaticClass())) HoldOne(ref::Ref(static_cast<APawn*>(a)->Controller).Get<AActor>(), n);
            }
        return n;
    }

    int ResumeNow() {
        int n = 0;
        for (const auto& h : g_held)
            if (AActor* a = h.key.Get<AActor>()) n += boss_intro::pause::Release(h, a->CustomTimeDilation);
        g_held.clear();
        return n;
    }

    void Sweep(std::uintptr_t fight, const float from[3], const float to[3]) {
        AcquireSRWLockExclusive(&g_mu);
        g_sweep = {{from[0], from[1], from[2]}, {to[0], to[1], to[2]}, fight};
        ReleaseSRWLockExclusive(&g_mu);
        g_sweepOn = true;
    }
    float Clear() { return g_clear.load(); }
    void StopSweep() { g_sweepOn = false; g_clear = 1.0f; g_bossWant = 0; }

    std::vector<Event> Take() {
        std::vector<Event> out;
        AcquireSRWLockExclusive(&g_mu);
        out.swap(g_events);
        ReleaseSRWLockExclusive(&g_mu);
        return out;
    }
}

namespace {
    // Game thread, once per camera update: the reads and writes the render thread asked for.
    void GameTick() {
        using namespace boss_intro::game_side;
        if (const std::uintptr_t fight = g_bossWant.load()) {
            BossRead r{fight, false, {}};
            r.ok = BossNow(fight, r.boss);
            AcquireSRWLockExclusive(&g_mu);
            g_boss = r;
            ReleaseSRWLockExclusive(&g_mu);
        }
        if (const std::uintptr_t fight = g_pauseFight.load()) g_paused += PauseNow(fight);
        if (int asked = 1; g_resumeReq.compare_exchange_strong(asked, 2)) {
            g_resumed = ResumeNow();
            g_resumeReq = 0;
        }
    }
}

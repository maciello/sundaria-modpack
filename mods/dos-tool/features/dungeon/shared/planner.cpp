#include "planner.hpp"
#include "probe.hpp"
#include "game.hpp"
#include "logger.hpp"
#include "ref.hpp"
#include "umg.hpp"

#include <Windows.h>
#include <atomic>
#include <cmath>
#include <string>
#include "Engine_classes.hpp"
#include "BP_DungeonFloor_classes.hpp"
#include "BP_DungeonFloor_parameters.hpp"
#include "BP_TriggerBase_classes.hpp"

// Facts: references/game-facts.md § Dungeon.
using namespace SDK;
using umg::PtrOk;

namespace {
    constexpr double kDoorSettle = 1.0;  // s after a door's last event: the navmesh updates once its collision changed (unverified)
    constexpr double kRetry = 5.0;       // s: no plan yet (dungeon still loading)

    std::atomic<unsigned> g_users{0};
    std::atomic<bool> g_fresh{false};  // a user came on: replan at the next world tick
    thread_local bool t_busy = false;

    struct Events {  // Blueprint classes: resolved only inside a dungeon (a StaticClass miss searches GObjects)
        ref::Fn floorOn{ABP_DungeonFloor_C::StaticClass, "BP_DungeonFloor_C", "I_SetFloorActivated"};
        ref::Fn floorEnter{ABP_DungeonFloor_C::StaticClass, "BP_DungeonFloor_C",
                           "BndEvt__FloorActivation_K2Node_ComponentBoundEvent_306_ComponentBeginOverlapSignature__DelegateSignature"};
        ref::Fn state{ABP_TriggerBase_C::StaticClass, "BP_TriggerBase_C", "OnTriggerStateChanged"};
        ref::Fn lock{ABP_TriggerBase_C::StaticClass, "BP_TriggerBase_C", "OnRep_LockStatus"};
    } g_ev;

    // Game thread state.
    ref::Ref g_gameState;
    bool g_inDungeon = false, g_havePawn = false;
    double g_replanAt = INFINITY;
    dungeon_map::Plan g_plan;
    int g_version = 0;
    dungeon_map::V3 g_pawn;

    // Game thread → render thread.
    SRWLOCK g_mu = SRWLOCK_INIT;
    dungeon_map::V3 g_pawnShown;
    std::vector<dungeon_map::V3> g_leversShown;
    void Publish(dungeon_map::V3 pawn, const std::vector<dungeon_map::V3>& levers) {
        AcquireSRWLockExclusive(&g_mu);
        g_pawnShown = pawn;
        g_leversShown = levers;
        ReleaseSRWLockExclusive(&g_mu);
    }

    double Now() { return double(GetTickCount64()) / 1000.0; }
    void ReplanIn(double s) { g_replanAt = std::min(g_replanAt, Now() + s); }

    // Entering or leaving a dungeon: the game state object changes (BP_GameState_Dungeon_C in a dungeon).
    void CheckGameState() {
        UWorld* w = UWorld::GetWorld();
        AGameStateBase* gs = PtrOk(w) ? w->GameState : nullptr;
        if (g_gameState.Is(gs)) return;
        g_gameState = ref::Ref(gs);
        g_inDungeon = PtrOk(gs) && PtrOk(gs->Class) && gs->Class->GetName().find("Dungeon") != std::string::npos;
        g_plan = {};
        g_version++;
        g_replanAt = g_inDungeon ? Now() : INFINITY;
    }

    void WorldTick() {
        CheckGameState();
        if (g_fresh.exchange(false) && g_inDungeon) ReplanIn(0);
        APlayerController* pc = umg::LocalPC();
        g_havePawn = g_inDungeon && pc && PtrOk(pc->Pawn);
        if (!g_havePawn) return Publish({}, {});
        const FVector l = pc->Pawn->K2_GetActorLocation();
        g_pawn = {l.X, l.Y, l.Z};
        if (Now() >= g_replanAt) {
            g_plan = dungeon_map::Compute(g_pawn);
            g_version++;
            g_replanAt = g_plan.ok ? INFINITY : Now() + kRetry;
            logger::log("[dungeon-map] plan: " + g_plan.why);
        }
        Publish(g_pawn, g_plan.ok && g_plan.marks.locked ? g_plan.marks.levers : std::vector<dungeon_map::V3>{});
    }

    void OnEvent(void* obj, void* fn, void* parms) {
        if (t_busy || !game::OnGameThread()) return;
        t_busy = true;
        if (umg::IsWorldTick(fn)) WorldTick();
        else if (g_inDungeon) {
            if (g_ev.state.Is(fn) || g_ev.lock.Is(fn)) g_replanAt = Now() + kDoorSettle;  // the last of a door's events counts
            else if (g_ev.floorOn.Is(fn)) ReplanIn(0);
            else if (g_ev.floorEnter.Is(fn)) {
                APlayerController* pc = umg::LocalPC();
                auto* p = static_cast<Params::BP_DungeonFloor_C_BndEvt__FloorActivation_K2Node_ComponentBoundEvent_306_ComponentBeginOverlapSignature__DelegateSignature*>(parms);
                if (pc && p && p->OtherActor == pc->Pawn) ReplanIn(0);  // the local player walked onto a floor
            }
            if (const std::string line = dungeon_map::probe::Event(obj, fn); !line.empty()) logger::log(line);
        }
        t_busy = false;
    }
}

namespace dungeon_map::planner {
    void Use(User u, bool on) {
        const unsigned before = on ? g_users.fetch_or(u) : g_users.fetch_and(~unsigned(u));
        const unsigned after = on ? before | u : before & ~unsigned(u);
        if (!before && after) { g_fresh = true; game::SetEventListener(&OnEvent, true); }
        if (before && !after) { game::SetEventListener(&OnEvent, false); Publish({}, {}); }
    }
    bool Using(User u) { return (g_users.load() & u) != 0; }
    bool InDungeon() { return g_inDungeon; }
    const Plan& Current() { return g_plan; }
    int Version() { return g_version; }
    bool Pawn(V3& out) { if (g_havePawn) out = g_pawn; return g_havePawn; }
    void Levers(V3& pawn, std::vector<V3>& at) {
        AcquireSRWLockShared(&g_mu);
        pawn = g_pawnShown;
        at = g_leversShown;
        ReleaseSRWLockShared(&g_mu);
    }
}

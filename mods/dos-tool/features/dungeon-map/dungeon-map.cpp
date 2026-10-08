#include "draw.hpp"
#include "plan.hpp"
#include "probe.hpp"
#include "feature.hpp"
#include "game.hpp"
#include "logger.hpp"
#include "ref.hpp"
#include "umg.hpp"

#include <Windows.h>
#include <atomic>
#include <map>
#include <string>
#include "Engine_classes.hpp"
#include "WidgetMinimap_classes.hpp"
#include "BP_DungeonFloor_classes.hpp"
#include "BP_DungeonFloor_parameters.hpp"
#include "BP_TriggerBase_classes.hpp"

// Dungeon map (#40): the floor's main path (navmesh, entry → stairs down) drawn on the game's minimap up to the local
// player's frontier, with a flow along it and blocked/lever icons where it stops (#66 #67 #68).
// Replans only on game events (door state, lock, floor activation) and on entering a dungeon; game thread only.
// Facts: references/game-facts.md § Dungeon, game-ui.md § Minimap. Dev probe: dungeon-map.probe next to the exe.
using namespace SDK;
using umg::PtrOk;

namespace {
    constexpr double kDoorSettle = 1.0;  // s after a door's last event: the navmesh updates once its collision changed (unverified)
    constexpr double kRetry = 5.0;       // s: no plan yet (dungeon still loading)

    std::atomic<bool> g_on{false}, g_probe{false};
    bool g_listening = false;
    thread_local bool t_busy = false;
    ref::Fn g_minimapTick{UWidgetMiniMap_C::StaticClass, "WidgetMiniMap_C", "Tick"};
    ref::Ref g_minimap;  // the live HUD minimap, from its own Tick (no search)

    // Game thread state.
    struct Events {  // Blueprint classes: resolved only inside a dungeon (a StaticClass miss searches GObjects)
        ref::Fn floorOn{ABP_DungeonFloor_C::StaticClass, "BP_DungeonFloor_C", "I_SetFloorActivated"};
        ref::Fn floorEnter{ABP_DungeonFloor_C::StaticClass, "BP_DungeonFloor_C",
                           "BndEvt__FloorActivation_K2Node_ComponentBoundEvent_306_ComponentBeginOverlapSignature__DelegateSignature"};
        ref::Fn state{ABP_TriggerBase_C::StaticClass, "BP_TriggerBase_C", "OnTriggerStateChanged"};
        ref::Fn lock{ABP_TriggerBase_C::StaticClass, "BP_TriggerBase_C", "OnRep_LockStatus"};
    } g_ev;
    ref::Ref g_gameState;
    bool g_inDungeon = false, g_drawn = false;
    double g_replanAt = INFINITY;
    dungeon_map::Plan g_plan;
    std::map<std::pair<int, int>, dungeon_map::Frontier> g_frontiers;  // (dungeon seed, floor number), this dungeon run

    double Now() { return double(GetTickCount64()) / 1000.0; }
    void ReplanIn(double s) { g_replanAt = std::min(g_replanAt, Now() + s); }

    std::string ExeDir() {
        char buf[MAX_PATH] = {};
        GetModuleFileNameA(nullptr, buf, MAX_PATH);
        const std::string p = buf;
        return p.substr(0, p.find_last_of("\\/") + 1);
    }

    void WriteProbe() {
        const std::string out = dungeon_map::probe::Report(g_minimap.Get<void>());
        HANDLE h = CreateFileA((ExeDir() + "dos-tool-dungeon.yaml").c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h == INVALID_HANDLE_VALUE) return;
        DWORD n = 0;
        WriteFile(h, out.data(), DWORD(out.size()), &n, nullptr);
        CloseHandle(h);
        logger::log("[dungeon-map] probe: " + std::to_string(out.size()) + " bytes -> dos-tool-dungeon.yaml");
    }

    // Entering or leaving a dungeon: the game state object changes (BP_GameState_Dungeon_C in a dungeon).
    void CheckGameState() {
        UWorld* w = UWorld::GetWorld();
        AGameStateBase* gs = PtrOk(w) ? w->GameState : nullptr;
        if (g_gameState.Is(gs)) return;
        g_gameState = ref::Ref(gs);
        g_inDungeon = PtrOk(gs) && PtrOk(gs->Class) && gs->Class->GetName().find("Dungeon") != std::string::npos;
        g_plan = {};
        g_frontiers.clear();
        g_replanAt = g_inDungeon ? Now() : INFINITY;
    }

    void WorldTick() {
        if (g_probe.exchange(false)) WriteProbe();
        CheckGameState();
        if (!g_on.load() || !g_inDungeon) {
            if (g_drawn) dungeon_map::draw::Clear();
            g_drawn = false;
            return;
        }
        APlayerController* pc = umg::LocalPC();
        if (!pc || !PtrOk(pc->Pawn)) return;
        const FVector l = pc->Pawn->K2_GetActorLocation();
        const dungeon_map::V3 pawn{l.X, l.Y, l.Z};
        if (Now() >= g_replanAt) {
            g_plan = dungeon_map::Compute(pawn);
            g_replanAt = g_plan.ok ? INFINITY : Now() + kRetry;
            logger::log("[dungeon-map] plan: " + g_plan.why);
        }
        if (!g_plan.ok) return;
        dungeon_map::Frontier& f = g_frontiers[{g_plan.seed, g_plan.floor}];
        f.Visit(g_plan.path, pawn);
        auto* m = g_minimap.Get<UWidgetMiniMap_C>();
        if (!m || m->UnitToPixel <= 0 || !PtrOk(m->CanvasPanel_Map)) return;
        const auto qs = dungeon_map::Scene(g_plan.path, f.S(g_plan.path), g_plan.marks, float(m->UnitToPixel), m->CanvasPanel_Map->RenderTransform.Angle, Now());
        dungeon_map::draw::Sync(m, qs);
        g_drawn = true;
    }

    void OnEvent(void* obj, void* fn, void* parms) {
        if (t_busy || !game::OnGameThread()) return;
        t_busy = true;
        const bool on = g_on.load(std::memory_order_relaxed);
        if (umg::IsWorldTick(fn)) WorldTick();
        else if (on && g_minimapTick.Is(fn)) {
            if (!g_minimap.Is(obj) && umg::Live(static_cast<UObject*>(obj))) g_minimap = ref::Ref(obj);
        } else if (on && g_inDungeon) {
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

    struct DungeonMap : feature::Feature {
        double next = 0;
        std::string dir;
        DungeonMap() : Feature("Dungeon map", feature::Stage::Alpha) { optIn = true; }  // new game-thread hook
        void OnFrame(const feature::Frame& f) override {
            g_on = true;
            if (!g_listening) game::SetEventListener(&OnEvent, g_listening = true);
            if (f.now < next) return;
            next = f.now + 1.0;
            if (dir.empty()) dir = ExeDir();
            const std::string p = dir + "dungeon-map.probe";
            if (GetFileAttributesA(p.c_str()) == INVALID_FILE_ATTRIBUTES) return;
            DeleteFileA(p.c_str());
            g_probe = true;
        }
        // ponytail: the listener stays registered (O(1) per event) so the next world tick removes our widgets;
        // unregister after that if the 8-listener cap gets tight.
        void Off() override { g_on = false; }
    } g_feature;
}

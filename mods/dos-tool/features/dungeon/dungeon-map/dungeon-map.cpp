#include "draw.hpp"
#include "../shared/lever.hpp"
#include "../shared/plan.hpp"
#include "../shared/probe.hpp"
#include "feature.hpp"
#include "game.hpp"
#include "logger.hpp"
#include "ref.hpp"
#include "drain.hpp"
#include "umg.hpp"
#include "style.hpp"
#include "../../../core/draw.hpp"
#include "imgui.h"

#include <Windows.h>
#include <algorithm>
#include <atomic>
#include <cfloat>
#include <cstdio>
#include <string>
#include "Engine_classes.hpp"
#include "WidgetMinimap_classes.hpp"
#include "BP_DungeonFloor_classes.hpp"
#include "BP_DungeonFloor_parameters.hpp"
#include "BP_TriggerBase_classes.hpp"
#include "UMG_classes.hpp"
#include "UMG_parameters.hpp"

// Dungeon map (#40): the floor's main route, entry → stairs down (route.hpp), drawn in full on the game's minimap with a flow
// along it and blocked/lever icons where a locked door cuts it (#66 #67 #68 #93). When the main route is out of the minimap's
// view, a dashed connector leads from the player to it (#83). The main route replans on game events (door state, lock,
// floor activation) and on entering a dungeon; the connector on a room change or when the player strays. Game thread only.
// Facts: references/game-facts.md § Dungeon, game-ui.md § Minimap. Dev probe: dungeon-map.probe next to the exe.
using namespace SDK;
using umg::PtrOk;

namespace {
    constexpr double kDoorSettle = 1.0;  // s after a door's last event: the navmesh updates once its collision changed (unverified)
    constexpr double kRetry = 5.0;       // s: no plan yet (dungeon still loading)
    constexpr double kReplanGap = 1.0;   // s: at most one replan per this when the player moves (room change, off the route)

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
    game::Drain g_drain;  // Off(): the game thread removes our overlay

    void RemoveAll() { dungeon_map::draw::Clear(); g_drawn = false; }  // game thread (or Off() after its wait ran out)
    double g_replanAt = INFINITY;
    dungeon_map::Plan g_plan;
    dungeon_map::Progress g_progress;  // along the main route, this floor
    dungeon_map::Path g_link;          // connector: player → main route, empty = none
    bool g_linkWanted = false;
    int g_room = -1;                   // the player's room when the connector was made
    double g_linkedAt = 0;
    float g_mapPx = 0;                 // minimap view size (px), read from the widget once per minimap
    constexpr float kMapPxFallback = 250;  // game-ui.md § Minimap SizeBox 250×250, used when the read fails (unverified)
    ref::Fn g_cachedGeo{UWidget::StaticClass, "Widget", "GetCachedGeometry"};
    ref::Fn g_localSize{USlateBlueprintLibrary::StaticClass, "SlateBlueprintLibrary", "GetLocalSize"};

    // The minimap's visible size in its own pixels: the retainer that clips it (game thread).
    float ReadMapPx(UWidgetMiniMap_C* m) {
        UFunction* g = g_cachedGeo.Get();
        UFunction* s = g_localSize.Get();
        if (!g || !s || !PtrOk(m->RetainerBox_Minimap)) return 0;
        Params::Widget_GetCachedGeometry q{};
        umg::CallNative(m->RetainerBox_Minimap, g, &q);
        Params::SlateBlueprintLibrary_GetLocalSize z{};
        z.Geometry = q.ReturnValue;
        umg::CallNative(USlateBlueprintLibrary::GetDefaultObj(), s, &z);
        return float(std::min(z.ReturnValue.X, z.ReturnValue.Y));
    }

    // Game thread → render thread (#93): the player and the levers on the route, plain copies.
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
        g_progress = {};
        g_link.clear();
        g_replanAt = g_inDungeon ? Now() : INFINITY;
    }

    void WorldTick() {
        if (g_probe.exchange(false)) WriteProbe();
        CheckGameState();
        if (!g_on.load() || !g_inDungeon) {
            Publish({}, {});
            if (g_drawn) dungeon_map::draw::Clear();
            g_drawn = false;
            return;
        }
        APlayerController* pc = umg::LocalPC();
        if (!pc || !PtrOk(pc->Pawn)) return;
        const FVector l = pc->Pawn->K2_GetActorLocation();
        const dungeon_map::V3 pawn{l.X, l.Y, l.Z};
        if (Now() >= g_replanAt) {
            const int floor = g_plan.floor;
            g_plan = dungeon_map::Compute(pawn);
            g_replanAt = g_plan.ok ? INFINITY : Now() + kRetry;
            if (g_plan.floor != floor) g_progress = {};
            g_link.clear();
            logger::log("[dungeon-map] plan: " + g_plan.why);
        }
        Publish(pawn, g_plan.ok && g_plan.marks.locked ? g_plan.marks.levers : std::vector<dungeon_map::V3>{});
        if (!g_plan.ok) return;
        auto* m = g_minimap.Get<UWidgetMiniMap_C>();
        if (!m || m->UnitToPixel <= 0 || !PtrOk(m->CanvasPanel_Map)) return;
        g_progress.Visit(g_plan.path, pawn);  // O(path points) per tick

        // Connector: only while no main-route point lies inside the minimap's view (radius of its inscribed circle).
        if (g_mapPx <= 0) {
            const float px = ReadMapPx(m);
            g_mapPx = px > 0 ? px : kMapPxFallback;
            logger::log("[dungeon-map] minimap view: " + std::to_string(int(g_mapPx)) + " px (" + (px > 0 ? "widget geometry" : "fallback") + ")");
        }
        const float scale = std::max(m->CanvasPanel_Map->RenderTransform.Scale.X, 0.01f);
        const float radius = 0.5f * g_mapPx / scale * float(m->UnitToPixel);
        const float off = dungeon_map::MapDist(g_plan.path, pawn);
        g_linkWanted = off > radius * (g_linkWanted ? dungeon_map::kLinkHide : 1.f);
        if (!g_linkWanted) g_link.clear();
        else {
            const int room = dungeon_map::RoomOf(g_plan.rooms, pawn);
            const bool stray = g_link.empty() || dungeon_map::Project(g_link, pawn).d > dungeon_map::kOffLine || (room >= 0 && room != g_room);
            if (stray && Now() >= g_linkedAt + kReplanGap) {
                const dungeon_map::V3 join = dungeon_map::Join(g_plan.path, pawn, g_progress.S(g_plan.path));
                g_link = dungeon_map::Connect(pawn, join);
                g_room = room;
                g_linkedAt = Now();
                char b[160];
                std::snprintf(b, sizeof b, "[dungeon-map] connector: %zu points, %.0f long, route %.0f off (view radius %.0f), joins at %.0f of %.0f",
                              g_link.size(), dungeon_map::Length(g_link), off, radius, dungeon_map::Project(g_plan.path, join).s, dungeon_map::Length(g_plan.path));
                logger::log(b);
            }
        }
        dungeon_map::Path link;
        if (!g_link.empty()) link = dungeon_map::Suffix(g_link, std::floor(dungeon_map::Project(g_link, pawn).s / dungeon_map::kStep) * dungeon_map::kStep);
        const auto qs = dungeon_map::Scene(g_plan.path, link, g_plan.marks, float(m->UnitToPixel), m->CanvasPanel_Map->RenderTransform.Angle, Now());
        dungeon_map::draw::Sync(m, qs);
        g_drawn = true;
    }

    void OnEvent(void* obj, void* fn, void* parms) {
        if (t_busy || !game::OnGameThread()) return;
        t_busy = true;
        if (umg::IsWorldTick(fn) && g_drain.Serve(RemoveAll)) { t_busy = false; return; }
        const bool on = g_on.load(std::memory_order_relaxed);
        if (umg::IsWorldTick(fn)) WorldTick();
        else if (on && g_minimapTick.Is(fn)) {
            if (!g_minimap.Is(obj) && umg::Live(static_cast<UObject*>(obj))) g_minimap = ref::Ref(obj), g_mapPx = 0;
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

    // World marker over a lever on the route (#93), render thread: our diamond over it, its distance below; off screen
    // the diamond sits on the screen edge with a chevron towards it. Spec: design-system.md § Dungeon map path (levers).
    void DrawLever(ImDrawList* dl, const feature::Frame& f, const combat::View& view, dungeon_map::V3 at, float dist, float alpha) {
        using namespace style;
        const float ui = type::Ui(f.h), r = dungeon_map::kMarkerPx * ui * 0.5f, o = stroke::Outline(type::kSm * ui);
        const dungeon_map::Marker m = dungeon_map::Place(view, {at.x, at.y, at.z + dungeon_map::kLeverLift}, f.w, f.h, space::k7 * ui);
        auto diamond = [&](float rr, ImU32 c) { dl->AddQuadFilled({m.x, m.y - rr}, {m.x + rr, m.y}, {m.x, m.y + rr}, {m.x - rr, m.y}, c); };
        diamond(r + o, Pack(color::kInk, alpha));
        diamond(r, Pack(color::kGameHighlight, alpha));
        if (m.edge) {  // chevron: two bars meeting at a tip beyond the diamond, towards the lever
            const float c = std::cos(m.angle), s = std::sin(m.angle), len = dungeon_map::kChevronLen * ui;
            const ImVec2 tip{m.x + c * (r + o + space::k3 * ui + len * 0.7f), m.y + s * (r + o + space::k3 * ui + len * 0.7f)};
            for (float side : {0.785f, -0.785f}) {
                const float a = m.angle + 3.14159265f + side;
                dl->AddLine(tip, {tip.x + std::cos(a) * len, tip.y + std::sin(a) * len}, Pack(color::kGameHighlight, alpha), dungeon_map::kChevronW * ui);
            }
            return;
        }
        char b[16];
        std::snprintf(b, sizeof b, "%.0f m", dist / 100);
        const float size = type::kSm * ui;
        const ImVec2 ts = f.font->CalcTextSizeA(size, FLT_MAX, 0, b);
        draw::OutlinedText(dl, f.font, size, {m.x - ts.x * 0.5f, m.y + r + o + space::k2 * ui}, Pack(color::kTextSoft, alpha),
                           Pack(color::kInk, alpha * stroke::kOutlineAlpha), o, b);
    }

    struct DungeonMap : feature::Feature {
        double next = 0, last = 0;
        std::string dir;
        struct Fade { dungeon_map::V3 at; float a = 0; bool want = false; };
        std::vector<Fade> fades;  // one per lever marker, render thread

        void Levers(const feature::Frame& f) {
            const float dt = last > 0 ? float(std::min(f.now - last, 0.1)) : 0;
            last = f.now;
            dungeon_map::V3 pawn;
            std::vector<dungeon_map::V3> ls;
            AcquireSRWLockShared(&g_mu);
            pawn = g_pawnShown;
            ls = g_leversShown;
            ReleaseSRWLockShared(&g_mu);
            for (Fade& x : fades) x.want = false;
            for (const dungeon_map::V3& l : ls) {
                auto it = std::find_if(fades.begin(), fades.end(), [&](const Fade& x) { return dungeon_map::Dist(x.at, l) < 50; });
                if (it == fades.end()) it = fades.insert(fades.end(), Fade{l});
                const float d = dungeon_map::Dist(pawn, l);
                it->want = d > dungeon_map::kLeverNear && d <= dungeon_map::kLeverRange;
            }
            for (Fade& x : fades)
                x.a = std::clamp(x.a + (x.want ? dt / style::motion::kFadeIn.dur : -dt / style::motion::kFadeOut.dur), 0.f, 1.f);
            std::erase_if(fades, [](const Fade& x) { return x.a <= 0 && !x.want; });
            combat::View view;
            if (fades.empty() || !game::GetView(view)) return;
            ImDrawList* dl = ImGui::GetBackgroundDrawList();  // Layer::WorldBar
            for (const Fade& x : fades) {
                const auto& c = x.want ? style::motion::kFadeIn : style::motion::kFadeOut;
                DrawLever(dl, f, view, x.at, dungeon_map::Dist(pawn, x.at), style::ease::Apply(c.curve, x.a));
            }
        }

        DungeonMap() : Feature("Dungeon map", feature::Stage::Alpha) { optIn = true; }  // new game-thread hook
        void OnFrame(const feature::Frame& f) override {
            g_on = true;
            if (!g_listening) game::SetEventListener(&OnEvent, g_listening = true);
            Levers(f);
            if (f.now < next) return;
            next = f.now + 1.0;
            if (dir.empty()) dir = ExeDir();
            const std::string p = dir + "dungeon-map.probe";
            if (GetFileAttributesA(p.c_str()) == INVALID_FILE_ATTRIBUTES) return;
            DeleteFileA(p.c_str());
            g_probe = true;
        }
        // The listener stays registered (O(1) per event). Off() runs with the ProcessEvent hook alive (#84):
        // the next world tick removes the overlay; the world-tick path clears it too once g_on is false.
        void Off() override {
            g_on = false;
            Publish({}, {});
            fades.clear();
            g_drain.Request(g_drawn, "dungeon-map", RemoveAll);
        }
    } g_feature;
}

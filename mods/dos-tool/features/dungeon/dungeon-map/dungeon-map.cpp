#include "draw.hpp"
#include "../shared/planner.hpp"
#include "../shared/probe.hpp"
#include "../shared/event.hpp"
#include "feature.hpp"
#include "game.hpp"
#include "logger.hpp"
#include "cost.hpp"
#include "ref.hpp"
#include "drain.hpp"
#include "umg.hpp"

#include <Windows.h>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <string>
#include "Engine_classes.hpp"
#include "WidgetMinimap_classes.hpp"
#include "UMG_classes.hpp"
#include "UMG_parameters.hpp"

// Dungeon map (#40): the floor's main route, entry → stairs down (shared/planner.hpp), drawn on the game's minimap from the
// furthest point the player has reached, with a flow along it and a blocked icon where a locked door cuts it (#66 #67 #68);
// the lever icons on it only while Lever markers is on (#93, planner::Using). When the main route is out of the minimap's view, a dashed connector leads from the player to it (#83), replanned on a room change or when the player
// strays. Game thread only. Facts: references/game-facts.md § Dungeon, game-ui.md § Minimap. Dev probe: dungeon-map.probe.
using namespace SDK;
using umg::PtrOk;

namespace {
    constexpr double kReplanGap = 1.0;     // s: at most one connector replan per this (room change, off the connector)
    constexpr float kMapPxFallback = 250;  // game-ui.md § Minimap SizeBox 250×250, used when the read fails (unverified)

    std::atomic<bool> g_on{false}, g_probe{false};
    bool g_listening = false;
    thread_local bool t_busy = false;
    dungeon_map::Event g_minimapTick{UWidgetMiniMap_C::StaticName, L"Tick"};
    ref::Ref g_minimap;  // the live HUD minimap, from its own Tick (no search)
    ref::Fn g_cachedGeo{UWidget::StaticClass, "Widget", "GetCachedGeometry"};
    ref::Fn g_localSize{USlateBlueprintLibrary::StaticClass, "SlateBlueprintLibrary", "GetLocalSize"};

    // Game thread state.
    bool g_drawn = false;
    game::Drain g_drain;  // Off(): the game thread removes our overlay
    int g_version = -1, g_floor = -1;  // of the plan the connector was made for
    dungeon_map::Progress g_progress;  // along the main route, this floor
    dungeon_map::Path g_link;          // connector: player → main route, empty = none
    bool g_linkWanted = false;
    int g_room = -1;                   // the player's room when the connector was made
    double g_linkedAt = 0;
    float g_mapPx = 0;                 // minimap view size (px), read from the widget once per minimap

    void RemoveAll() { dungeon_map::draw::Clear(); g_drawn = false; }  // game thread (Drain)
    double Now() { return double(GetTickCount64()) / 1000.0; }

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

    // The connector: only while no main-route point lies inside the minimap's view (radius of its inscribed circle).
    // ahead = the main route from the furthest point reached: what the map shows.
    dungeon_map::Path Connector(const dungeon_map::Plan& plan, const dungeon_map::Path& ahead, dungeon_map::V3 pawn, UWidgetMiniMap_C* m) {
        if (g_mapPx <= 0) {
            const float px = ReadMapPx(m);
            g_mapPx = px > 0 ? px : kMapPxFallback;
            logger::log("[dungeon-map] minimap view: " + std::to_string(int(g_mapPx)) + " px (" + (px > 0 ? "widget geometry" : "fallback") + ")");
        }
        const float scale = std::max(m->CanvasPanel_Map->RenderTransform.Scale.X, 0.01f);
        const float radius = 0.5f * g_mapPx / scale * float(m->UnitToPixel);
        const float off = dungeon_map::MapDist(ahead, pawn);
        g_linkWanted = off > radius * (g_linkWanted ? dungeon_map::kLinkHide : 1.f);
        if (!g_linkWanted) { g_link.clear(); return {}; }
        const int room = dungeon_map::RoomOf(plan.rooms, pawn);
        const bool stray = g_link.empty() || dungeon_map::Project(g_link, pawn).d > dungeon_map::kOffLine || (room >= 0 && room != g_room);
        if (stray && Now() >= g_linkedAt + kReplanGap) {
            const dungeon_map::V3 join = dungeon_map::Join(plan.path, pawn, g_progress.S(plan.path));
            static cost::Path linkCost{"dungeon connector query"};
            {
                cost::Scope cs(linkCost);  // one navmesh query; logged at its first call and every new max
                g_link = dungeon_map::Connect(pawn, join);
            }
            g_room = room;
            g_linkedAt = Now();
            char b[160];
            std::snprintf(b, sizeof b, "[dungeon-map] connector: %zu points, %.0f long, route %.0f off (view radius %.0f), joins at %.0f of %.0f",
                          g_link.size(), dungeon_map::Length(g_link), off, radius, dungeon_map::Project(plan.path, join).s, dungeon_map::Length(plan.path));
            logger::log(b);
        }
        if (g_link.empty()) return {};
        return dungeon_map::Suffix(g_link, std::floor(dungeon_map::Project(g_link, pawn).s / dungeon_map::kStep) * dungeon_map::kStep);
    }

    void WorldTick() {
        if (g_probe.exchange(false)) WriteProbe();
        const dungeon_map::Plan& plan = dungeon_map::planner::Current();
        dungeon_map::V3 pawn;
        if (!g_on.load() || !dungeon_map::planner::InDungeon() || !plan.ok || !dungeon_map::planner::Pawn(pawn)) {
            if (g_drawn) dungeon_map::draw::Clear();
            g_drawn = false;
            return;
        }
        if (dungeon_map::planner::Version() != g_version) {  // a new main route: the connector follows it
            g_version = dungeon_map::planner::Version();
            if (plan.floor != g_floor) g_progress = {}, g_floor = plan.floor;
            g_link.clear();
        }
        auto* m = g_minimap.Get<UWidgetMiniMap_C>();
        if (!m || m->UnitToPixel <= 0 || !PtrOk(m->CanvasPanel_Map)) return;
        g_progress.Visit(plan.path, pawn);  // O(path points) per tick
        const dungeon_map::Path ahead = dungeon_map::Suffix(plan.path, g_progress.S(plan.path));  // the walked part is gone
        const dungeon_map::Path link = Connector(plan, ahead, pawn, m);
        dungeon_map::Marks marks = plan.marks;  // lever icons + halo belong to the Lever markers toggle (#93)
        if (!dungeon_map::planner::Using(dungeon_map::planner::kLevers)) marks.levers.clear();
        const auto qs = dungeon_map::Scene(ahead, link, marks, float(m->UnitToPixel), m->CanvasPanel_Map->RenderTransform.Angle, Now());
        dungeon_map::draw::Sync(m, qs);
        g_drawn = true;
    }

    void OnEvent(void* obj, void* fn, void*) {
        if (t_busy || !game::OnGameThread()) return;
        t_busy = true;
        if (umg::IsWorldTick(fn) && g_drain.Serve(RemoveAll)) { t_busy = false; return; }
        if (umg::IsWorldTick(fn)) WorldTick();
        else if (g_on.load(std::memory_order_relaxed) && g_minimapTick.Is(fn)) {
            if (!g_minimap.Is(obj) && umg::Live(static_cast<UObject*>(obj))) g_minimap = ref::Ref(obj), g_mapPx = 0;
        }
        t_busy = false;
    }

    struct DungeonMap : feature::Feature {
        double next = 0;
        std::string dir;
        DungeonMap() : Feature("Dungeon map", feature::Stage::Alpha) { optIn = true; }  // new game-thread hook
        void OnFrame(const feature::Frame& f) override {
            g_on = true;
            dungeon_map::planner::Use(dungeon_map::planner::kMap, true);
            if (!g_listening) {
                g_listening = true;
                game::On("WidgetMiniMap_C", "Tick", &OnEvent, true);
                game::OnWorldTick(&OnEvent, true);
            }
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
            dungeon_map::planner::Use(dungeon_map::planner::kMap, false);
            g_drain.Request(g_drawn, "dungeon-map", RemoveAll);
        }
    } g_feature;
}

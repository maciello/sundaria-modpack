#include "plan.hpp"
#include "route.hpp"
#include "lever.hpp"
#include "logger.hpp"
#include "ref.hpp"
#include "umg.hpp"

#include <Windows.h>
#include <cstdarg>
#include <cstdio>
#include "Engine_classes.hpp"
#include "BP_GameState_classes.hpp"
#include "BP_Dungeon_classes.hpp"
#include "BP_DungeonFloor_classes.hpp"
#include "BP_BreadSlice_classes.hpp"
#include "BP_TriggerBase_classes.hpp"
#include "BP_Door_classes.hpp"
#include "BP_DeveloperLever_classes.hpp"
#include "BP_DungeonExitVolume_classes.hpp"
#include "NavigationSystem_classes.hpp"
#include "NavigationSystem_parameters.hpp"

// Reaches everything through owners: world → game state → DungeonActor → floors → chunk actors (rooms) → their
// trigger child actors, plus the floor's spawned actors (doors). No GObjects walk: Blueprint classes are matched by FName
// (StaticName); their StaticClass() searches GObjects on first use. Facts: game-facts.md § Dungeon.
using namespace SDK;
using umg::PtrOk;

namespace {
    ref::Fn g_findPath{UNavigationSystemV1::StaticClass, "NavigationSystemV1", "FindPathToLocationSynchronously"};
    ref::Fn g_project{UNavigationSystemV1::StaticClass, "NavigationSystemV1", "K2_ProjectPointToNavigation"};
    ref::Fn g_isPartial{UNavigationPath::StaticClass, "NavigationPath", "IsPartial"};

    dungeon_map::V3 W(const FVector& v) { return {v.X, v.Y, v.Z}; }
    dungeon_map::V3 Loc(AActor* a) { return W(a->K2_GetActorLocation()); }

    dungeon_map::Box BoxOf(Abp_breadslice_C* s) {
        UBoxComponent* b = s->DiscoveryBounds;
        if (!PtrOk(b)) return {};
        const FVector c = b->K2_GetComponentLocation(), sc = b->K2_GetComponentScale(), e = b->BoxExtent;
        return {W(c), {e.X * sc.X, e.Y * sc.Y, e.Z * sc.Z}, b->K2_GetComponentRotation().Yaw};
    }

    // Doors and levers; anything else (chests, volumes …⊇) is not a trigger of the path.
    bool AddTrigger(std::vector<dungeon_map::Trigger>& out, AActor* a, int room) {
        if (!PtrOk(a) || !a->IsA(ABP_TriggerBase_C::StaticName())) return false;
        const bool door = a->IsA(ABP_Door_C::StaticName()), lever = a->IsA(ABP_DeveloperLever_C::StaticName());
        if (!door && !lever) return false;
        auto* t = static_cast<ABP_TriggerBase_C*>(a);
        out.push_back({Loc(a), room, door, lever, int(t->mOpenCloseAnimState) == 0, int(t->LockStatus) != 0});
        return true;
    }

    struct Floor {
        ABP_DungeonFloor_C* actor = nullptr;
        std::vector<Abp_breadslice_C*> slices;  // index = room
        std::vector<dungeon_map::Box> rooms;
    };
    Floor Read(ABP_DungeonFloor_C* f) {
        Floor o{f};
        for (int j = 0; j < f->ChunkActors.Num(); j++)
            if (AActor* c = f->ChunkActors[j]; PtrOk(c) && c->IsA(Abp_breadslice_C::StaticName())) {
                o.slices.push_back(static_cast<Abp_breadslice_C*>(c));
                o.rooms.push_back(BoxOf(o.slices.back()));
            }
        return o;
    }

    // Stairs-down room, else the floor's exit volume (last floor). Null = neither.
    AActor* Goal(const Floor& f, bool& exitVolume) {
        exitVolume = false;
        for (Abp_breadslice_C* s : f.slices)
            if (PtrOk(s->Class) && s->Class->GetName().find("Stairs_Down") != std::string::npos) return s;
        for (int j = 0; j < f.actor->ChunkSpawnedActors.Num(); j++)
            if (AActor* a = f.actor->ChunkSpawnedActors[j]; PtrOk(a) && a->IsA(ABP_DungeonExitVolume_C::StaticName())) return exitVolume = true, a;
        return nullptr;
    }

    // The game's own synchronous navmesh query (≈0.2–0.3 ms, game-facts.md). Closed doors cut it: partial.
    bool NavPath(UObject* world, dungeon_map::V3 a, dungeon_map::V3 b, dungeon_map::Path& out, bool& partial) {
        UFunction* fn = g_findPath.Get();
        if (!fn) return false;
        Params::NavigationSystemV1_FindPathToLocationSynchronously q{};
        q.WorldContextObject = world;
        q.PathStart = {a.x, a.y, a.z};
        q.PathEnd = {b.x, b.y, b.z};
        umg::CallNative(UNavigationSystemV1::GetDefaultObj(), fn, &q);
        UNavigationPath* np = q.ReturnValue;
        if (!PtrOk(np) || np->PathPoints.Num() < 2) return false;
        for (int i = 0; i < np->PathPoints.Num(); i++) out.push_back(W(np->PathPoints[i]));
        Params::NavigationPath_IsPartial p{};
        if (UFunction* ip = g_isPartial.Get()) umg::CallNative(np, ip, &p);
        partial = p.ReturnValue || dungeon_map::Dist(out.back(), b) > dungeon_map::kPartial;
        return true;
    }

    std::string F(const char* fmt, ...);

    // Navmesh projection of p (extent 100/100/500): offset from p, or "off" when nothing lies within the extent.
    std::string OnNav(UObject* world, dungeon_map::V3 p) {
        UFunction* fn = g_project.Get();
        if (!fn) return "n/a";
        Params::NavigationSystemV1_K2_ProjectPointToNavigation q{};
        q.WorldContextObject = world;
        q.Point = {p.x, p.y, p.z};
        q.QueryExtent = {100, 100, 500};
        umg::CallNative(UNavigationSystemV1::GetDefaultObj(), fn, &q);
        if (!q.ReturnValue) return "off";
        return F("on(%.0f,%.0f,%.0f)", q.ProjectedLocation.X - p.x, q.ProjectedLocation.Y - p.y, q.ProjectedLocation.Z - p.z);
    }
    std::string Pt(dungeon_map::V3 v) { return F("(%.0f,%.0f,%.0f)", v.x, v.y, v.z); }

    // A navmesh point in the room: nearest to its centre within its box (route.hpp resumes there).
    bool Anchor(UObject* world, const dungeon_map::Box& r, dungeon_map::V3& out) {
        UFunction* fn = g_project.Get();
        if (!fn) return false;
        Params::NavigationSystemV1_K2_ProjectPointToNavigation q{};
        q.WorldContextObject = world;
        q.Point = {r.c.x, r.c.y, r.c.z};
        const float e = std::max(r.e.x, r.e.y);  // box yaw: the square around it
        q.QueryExtent = {e, e, r.e.z};
        umg::CallNative(UNavigationSystemV1::GetDefaultObj(), fn, &q);
        if (!q.ReturnValue) return false;
        out = W(q.ProjectedLocation);
        return true;
    }

    std::string F(const char* fmt, ...) {
        char b[768];
        va_list v;
        va_start(v, fmt);
        std::vsnprintf(b, sizeof b, fmt, v);
        va_end(v);
        return b;
    }
}

namespace {
    dungeon_map::Nav NavOf(UWorld* w) {
        return {[w](dungeon_map::V3 a, dungeon_map::V3 b, dungeon_map::Path& out, bool& partial) { return NavPath(w, a, b, out, partial); },
                [w](const dungeon_map::Box& r, dungeon_map::V3& out) { return Anchor(w, r, out); }};
    }
}

namespace dungeon_map {
    void Warm() {
        for (const FName* n : {&ABP_GameState_C::StaticName(), &ABP_Dungeon_C::StaticName(), &ABP_DungeonFloor_C::StaticName(),
                               &Abp_breadslice_C::StaticName(), &ABP_TriggerBase_C::StaticName(), &ABP_Door_C::StaticName(),
                               &ABP_DeveloperLever_C::StaticName(), &ABP_DungeonExitVolume_C::StaticName()})
            (void)n;
        g_findPath.Get(), g_project.Get(), g_isPartial.Get();
    }

    bool Begin(V3 pawn, Planning& out) {
        out = {};
        Plan& p = out.plan;
        UWorld* w = UWorld::GetWorld();
        AGameStateBase* gs = PtrOk(w) ? w->GameState : nullptr;
        if (!PtrOk(gs) || !gs->IsA(ABP_GameState_C::StaticName())) return p.why = "no dungeon game state", false;
        AActor* da = static_cast<ABP_GameState_C*>(gs)->DungeonActor;
        if (!PtrOk(da) || !da->IsA(ABP_Dungeon_C::StaticName())) return p.why = "no dungeon actor", false;
        auto* d = static_cast<ABP_Dungeon_C*>(da);

        // The pawn's floor: the one with a room around it, else the dungeon's active floor (corridor, stairs).
        Floor floor;
        for (int i = 0; i < d->FloorActors.Num() && !floor.actor; i++) {
            AActor* fa = d->FloorActors[i];
            if (!PtrOk(fa) || !fa->IsA(ABP_DungeonFloor_C::StaticName())) continue;
            Floor f = Read(static_cast<ABP_DungeonFloor_C*>(fa));
            if (RoomOf(f.rooms, pawn) >= 0) floor = std::move(f);
        }
        for (int i = 0; i < d->FloorActors.Num() && !floor.actor; i++) {
            AActor* fa = d->FloorActors[i];
            if (PtrOk(fa) && fa->IsA(ABP_DungeonFloor_C::StaticName()) && static_cast<ABP_DungeonFloor_C*>(fa)->FloorNumber == d->CurrentActiveFloor)
                floor = Read(static_cast<ABP_DungeonFloor_C*>(fa));
        }
        if (!floor.actor) return p.why = F("no floor for the pawn (active floor %d)", d->CurrentActiveFloor), false;
        p.floor = floor.actor->FloorNumber;
        AActor* goal = Goal(floor, out.exitVolume);
        if (!goal) return p.why = F("floor %d: no stairs down or exit volume", p.floor), false;
        out.goal = Loc(goal);
        p.rooms = floor.rooms;
        for (int r = 0; r < int(floor.slices.size()); r++)
            for (int j = 0; j < floor.slices[r]->TriggerBaseComponents.Num(); j++)
                if (UChildActorComponent* c = floor.slices[r]->TriggerBaseComponents[j]; PtrOk(c)) AddTrigger(out.ts, c->ChildActor, r);
        for (int j = 0; j < floor.actor->ChunkSpawnedActors.Num(); j++)
            if (AActor* a = floor.actor->ChunkSpawnedActors[j]; AddTrigger(out.ts, a, -1)) out.ts.back().room = RoomOf(floor.rooms, out.ts.back().at);
        out.route.Start(Loc(floor.actor), out.goal, floor.rooms);  // floor actor = its entry door
        return true;
    }

    bool Step(Planning& s) {
        UWorld* w = UWorld::GetWorld();
        const Nav nav = NavOf(w);
        Plan& p = s.plan;
        if (!s.route.done) {
            if (!s.route.Step(nav)) return false;
            const Route& r = s.route.r;
            p.path = r.path;
            p.partial = !r.stops.empty();
            if (p.partial) {  // why each leg stopped short (#83): a closed door in front of it, else a navmesh island
                std::string why = F("goal %s %s on-navmesh %s; stops:", s.exitVolume ? "exit-volume" : "stairs-room", Pt(s.goal).c_str(), OnNav(w, s.goal).c_str());
                for (size_t k = 0; k < r.stops.size(); k++) {
                    const int sd = StoppingDoor(s.ts, r.stops[k]);
                    why += F(" %s room %d: %s, nearest door %.0f;", Pt(r.stops[k]).c_str(), NearestRoom(p.rooms, r.stops[k]),
                             sd < 0 ? "navmesh island" : s.ts[sd].locked ? "locked door" : "closed door", DoorsAround(s.ts, r.stops[k]).nearestDoor);
                    if (s.door < 0 && sd >= 0) s.door = sd, s.detourAt = r.stopAt[k];
                }
                logger::log("[dungeon-map] partial: " + why);
                p.marks = MarksFor(s.ts, s.door);
            }
            if (p.marks.locked && !p.marks.levers.empty()) {  // the way runs through the levers first (#93)
                const V3 stop = p.path[s.detourAt];
                s.detour.Start(stop, LeverOrder(stop, p.marks.levers));
                s.detouring = true;
                return false;
            }
        } else if (s.detouring) {
            if (!s.detour.Step(nav)) return false;
            p.path = Splice(p.path, s.detourAt, s.detour.path);
        }
        p.ok = true;
        p.why = F("floor %d: %zu points, %.0f long, legs %d, stops %zu, stopping door %d, locked %d, levers %zu on route", p.floor, p.path.size(),
                  Length(p.path), s.route.r.legs + s.detour.legs, s.route.r.stops.size(), s.door >= 0, p.marks.locked, p.marks.levers.size());
        return true;
    }

    Path Connect(V3 from, V3 to) {
        Path p;
        bool partial = false;
        if (!NavPath(UWorld::GetWorld(), from, to, p, partial)) p = {from};
        Append(p, to);
        return p;
    }
}

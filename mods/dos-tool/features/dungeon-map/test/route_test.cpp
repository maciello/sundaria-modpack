#include "../route.hpp"
#include <cassert>
#include <cstdio>

using namespace dungeon_map;

static bool Near(float a, float b) { return std::abs(a - b) < 0.01f; }

int main() {
    // Four rooms along +X, 1000 wide: [0,1000) [1000,2000) [2000,3000) [3000,4000). Goal in the last one.
    const std::vector<Box> rooms{{{500, 0, 0}, {500, 500, 500}, 0}, {{1500, 0, 0}, {500, 500, 500}, 0},
                                 {{2500, 0, 0}, {500, 500, 500}, 0}, {{3500, 0, 0}, {500, 500, 500}, 0}};
    const V3 goal{3800, 0, 0};
    assert(NearestRoom(rooms, {2100, 0, 0}) == 2 && NearestRoom(rooms, {-300, 0, 0}) == 0 && NearestRoom({}, {}) == -1);
    assert(Near(BoxDist(rooms[0], {1300, 0, 0}), 300) && BoxDist(rooms[1], {1300, 0, 0}) == 0);

    // Navmesh with a cut at x = 1700 (an island edge or a locked door): legs starting before it stop at 1650.
    int queries = 0;
    Nav cut{[&](V3 a, V3 b, Path& out, bool& partial) {
                queries++;
                partial = a.x < 1700 && b.x > 1700;
                out = {a, {partial ? 1650.f : b.x, 0, 0}};
                return true;
            },
            [](const Box& r, V3& out) { out = r.c; return true; }};

    // Whole floor connected: one leg, no stops.
    Route r = PlanRoute({2100, 0, 0}, goal, rooms, cut);
    assert(r.legs == 1 && r.stops.empty() && Near(r.path.back().x, 3800));

    // Cut ahead: stop at 1650, resume in the next room after the stop (room 2 centre), on to the goal.
    r = PlanRoute({200, 0, 0}, goal, rooms, cut);
    assert(r.legs == 2 && r.stops.size() == 1 && Near(r.stops[0].x, 1650));
    assert(r.path.size() == 4 && Near(r.path[2].x, 2500) && Near(r.path.back().x, 3800));

    // Never resumes behind the player: a leg that ends in an earlier room resumes after the player's room.
    Nav back{[](V3 a, V3, Path& out, bool& partial) { partial = a.x < 2000; out = {a, {partial ? 300.f : 3800.f, 0, 0}}; return true; },
             [](const Box& r, V3& out) { out = r.c; return true; }};
    r = PlanRoute({1200, 0, 0}, goal, rooms, back);
    assert(r.stops.size() == 1 && Near(r.path[2].x, 2500));

    // Rooms without navmesh are skipped; no navmesh anywhere: straight to the goal, bounded by the room count.
    queries = 0;
    Nav none{[&](V3, V3, Path&, bool&) { queries++; return false; }, [](const Box& r, V3& out) { out = r.c; return r.c.x > 3000; }};
    r = PlanRoute({200, 0, 0}, goal, rooms, none);
    assert(queries == 2 && r.legs == 2 && Near(r.path[1].x, 3500) && Near(r.path.back().x, 3800));
    Nav dead{[](V3, V3, Path&, bool&) { return false; }, [](const Box& r, V3& out) { out = r.c; return true; }};
    r = PlanRoute({200, 0, 0}, goal, rooms, dead);
    assert(r.legs == 4 && Near(r.path.back().x, 3800));
    std::puts("ok");
}

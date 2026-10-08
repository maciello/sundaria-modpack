#include "../route.hpp"
#include <cassert>
#include <cstdio>

using namespace dungeon_map;

int main() {
    // 0 - 1 - 2 - 3 (goal), plus a long detour 0 - 4 - 3 and a dead end 1 - 5.
    Graph g(6);
    g[0].c = {0, 0}, g[1].c = {100, 0}, g[2].c = {200, 0}, g[3].c = {300, 0}, g[4].c = {150, 900}, g[5].c = {100, -100};
    Link(g, 0, 1, {50, 0});
    Link(g, 1, 2, {150, 0});
    Link(g, 2, 3, {250, 0});
    Link(g, 0, 4, {0, 500});
    Link(g, 4, 3, {300, 500});
    Link(g, 1, 5, {100, -50});
    const std::vector<int> r = Route(g, 0, 3);
    assert((r == std::vector<int>{0, 1, 2, 3}));
    assert(Route(g, 0, 0) == std::vector<int>{0});
    Graph lone(2);
    assert(Route(lone, 0, 1).empty());
    assert(Route(g, 0, 9).empty());

    // Line: start, doors up to the frontier room, its centre.
    const std::vector<P> l = Line(g, r, 2, {-10, 0});
    assert(l.size() == 4 && l[0].x == -10 && l[1].x == 50 && l[2].x == 150 && l[3].x == 200);
    assert(Line(g, r, -1, {}).empty());
    assert(Line(g, r, 0, {}).size() == 2);

    // Frontier: advances to the furthest visited route room, ignores rooms off the route, never moves back.
    Frontier f;
    assert(!f.Visit(5, r) && f.reach == -1);  // dead end: not on the route
    assert(f.Visit(0, r) && f.reach == 0);
    assert(f.Visit(2, r) && f.reach == 2);    // skipped room 1 (walked around): still the furthest point
    assert(!f.Visit(1, r) && f.reach == 2);   // backtracking keeps it
    assert(!f.Visit(0, r) && f.reach == 2);
    assert(!f.Rescan({0, 4, 3}) && f.reach == 2);  // shorter new route: kept, never back
    assert(f.Visit(3, r) && f.reach == 3);

    // Room boxes, rotated.
    const Box b{{0, 0}, {100, 10}, 90, 0, 200};
    assert(Inside(b, {0, 90}, 0) && !Inside(b, {90, 0}, 0) && !Inside(b, {0, 90}, 500));
    std::puts("ok");
}

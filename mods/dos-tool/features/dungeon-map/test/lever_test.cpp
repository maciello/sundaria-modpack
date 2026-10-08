#include "../lever.hpp"
#include "../route.hpp"
#include "../scene.hpp"
#include <cassert>
#include <cstdio>

using namespace dungeon_map;

static bool Near(float a, float b, float e = 0.01f) { return std::abs(a - b) < e; }

int main() {
    // Order: nearest first, then nearest to that one.
    const auto o = LeverOrder({0, 0, 0}, {{900, 0, 0}, {100, 0, 0}, {1000, 50, 0}});
    assert(o.size() == 3 && Near(o[0].x, 100) && Near(o[1].x, 900) && Near(o[2].x, 1000) && LeverOrder({}, {}).empty());

    // Route via the levers, then on to the goal; an unreachable lever is joined straight.
    const std::vector<Box> rooms{{{500, 0, 0}, {500, 500, 500}, 0}, {{1500, 0, 0}, {500, 500, 500}, 0}};
    Nav nav{[](V3 a, V3 b, Path& out, bool& partial) { partial = b.y > 0; out = {a, partial ? a : b}; return true; },
            [](const Box& r, V3& out) { out = r.c; return true; }};
    const Route r = PlanVia({0, 0, 0}, {{300, 0, 0}, {600, 400, 0}}, {1800, 0, 0}, rooms, nav);
    assert(r.path.size() == 4 && Near(r.path[1].x, 300) && Near(r.path[2].y, 400) && Near(r.path[3].x, 1800) && r.stops.empty());

    // Marker: camera at origin looking +X, 90° FOV, 1000×500 screen, inset 20.
    const combat::View v{0, 0, 0, 0, 0, 0, 90};
    Marker m = Place(v, {1000, 0, 0}, 1000, 500, 20);
    assert(!m.edge && Near(m.x, 500) && Near(m.y, 250));
    m = Place(v, {1000, 5000, 0}, 1000, 500, 20);  // far right: right edge, pointing right
    assert(m.edge && Near(m.x, 980) && Near(m.y, 250) && Near(m.angle, 0));
    m = Place(v, {-1000, 0, 500}, 1000, 500, 20);  // behind and above: top edge, pointing up
    assert(m.edge && Near(m.y, 20) && Near(m.angle, -1.5708f, 1e-3f));
    m = Place(v, {-1000, -300, 0}, 1000, 500, 20);  // behind, to the left: left edge
    assert(m.edge && Near(m.x, 20));

    // Minimap halo breathes 0 → kHaloAlpha → 0; a locked door's lever gets it under its plate.
    assert(Near(Halo(0, 1.6f), 0) && Near(Halo(0.8, 1.6f), kHaloAlpha) && Near(Halo(1.6, 1.6f), 0, 1e-4f));
    Marks mk;
    mk.locked = true;
    mk.levers = {{100, 0, 0}};
    int halos = 0;
    for (const Quad& q : Scene({}, -1, mk, 64, 0, 0.8)) halos += q.z == kZMark && Near(q.alpha, kHaloAlpha);
    assert(halos == 1);
    std::puts("ok");
}

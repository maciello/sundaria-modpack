#include "../../shared/lever.hpp"
#include "../../shared/route.hpp"
#include "../scene.hpp"
#include <cassert>
#include <cstdio>

using namespace dungeon_map;

static bool Near(float a, float b, float e = 0.01f) { return std::abs(a - b) < e; }

int main() {
    // Order: nearest first, then nearest to that one.
    const auto o = LeverOrder({0, 0, 0}, {{900, 0, 0}, {100, 0, 0}, {1000, 50, 0}});
    assert(o.size() == 3 && Near(o[0].x, 100) && Near(o[1].x, 900) && Near(o[2].x, 1000) && LeverOrder({}, {}).empty());

    // Detour from a stop through the levers and back, spliced into the route; an unreachable lever is joined straight.
    Nav nav{[](V3 a, V3 b, Path& out, bool& partial) { partial = b.y > 0; out = {a, partial ? a : b}; return true; },
            [](const Box& r, V3& out) { out = r.c; return true; }};
    DetourJob d;
    d.Start({300, 0, 0}, {{300, -200, 0}, {600, 400, 0}});
    assert(!d.Step(nav) && !d.Step(nav) && d.Step(nav) && d.legs == 3);
    assert(d.path.size() == 4 && Near(d.path[1].y, -200) && Near(d.path[2].y, 400) && Near(d.path[3].x, 300));
    const Path route{{0, 0, 0}, {300, 0, 0}, {1800, 0, 0}};
    const Path sp = Splice(route, 1, d.path);
    assert(sp.size() == 6 && Near(sp[1].x, 300) && Near(sp[2].y, -200) && Near(sp[4].x, 300) && Near(sp[5].x, 1800));

    // Minimap halo breathes 0 → kHaloAlpha → 0; a locked door's lever gets it under its plate.
    assert(Near(Halo(0, 1.6f), 0) && Near(Halo(0.8, 1.6f), kHaloAlpha) && Near(Halo(1.6, 1.6f), 0, 1e-4f));
    Marks mk;
    mk.locked = true;
    mk.levers = {{100, 0, 0}};
    int halos = 0;
    for (const Quad& q : Scene({}, {}, mk, 64, 0, 0.8)) halos += q.z == kZMark && Near(q.alpha, kHaloAlpha);
    assert(halos == 1);
    std::puts("ok");
}

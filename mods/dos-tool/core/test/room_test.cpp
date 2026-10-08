// just test
#include "room.hpp"
#include <cassert>
#include <cmath>
#include <cstdio>

static bool Near(float a, float b) { return std::fabs(a - b) < 0.01f; }

int main() {
    // 1000 x 600 building, floor at z 50, walls 400 high, 20 thick, inset 100
    auto b = room::Shell(0, 0, 1000, 600, 50, {100, 100, 100, 100}, 20, 400);
    assert(b.size() == 5);
    const room::Box& f = b[0];
    assert(Near(f.cx, 500) && Near(f.cy, 300) && Near(f.cz + f.ez, 50));   // floor top = floorZ
    assert(Near(f.ex, 400) && Near(f.ey, 200));                            // inset on every side
    const room::Box& w = b[1];                                            // -X wall
    assert(Near(w.cx - w.ex, 100) && Near(w.cz - w.ez, 50) && Near(w.cz + w.ez, 450) && Near(w.ey, 200));
    const room::Box& e = b[2];                                            // +X wall flush with the inset edge
    assert(Near(e.cx + e.ex, 900));
    const room::Box& s = b[3];
    assert(Near(s.cy - s.ey, 100) && Near(s.ex, 400));
    // too small after inset: nothing
    assert(room::Shell(0, 0, 100, 100, 0, {40, 40, 40, 40}, 20, 300).empty());
    // per-side insets move each wall on its own
    auto u = room::Shell(0, 0, 1000, 600, 0, {0, 200, 50, 0}, 20, 400);
    assert(Near(u[1].cx - u[1].ex, 0) && Near(u[2].cx + u[2].ex, 800) && Near(u[3].cy - u[3].ey, 50) && Near(u[4].cy + u[4].ey, 600));
    assert(Near(u[0].cx, 400) && Near(u[0].ex, 400) && Near(u[0].cy, 325));
    assert(room::Inside(10, 10, -250, 0, 0, 0, 100, 100, 500) && !room::Inside(10, 10, -400, 0, 0, 0, 100, 100, 500));
    assert(!room::Inside(-1, 10, 0, 0, 0, 0, 100, 100, 500));
    std::puts("ok");
}

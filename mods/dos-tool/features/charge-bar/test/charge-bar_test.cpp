// just test
#include "charge-bar.hpp"
#include <cassert>
#include <cmath>
#include <cstdio>

using namespace charge_bar;

static bool Near(float a, float b) { return std::fabs(a - b) < 1e-3f; }

int main() {
    // cast time 2 s: fills, ready punch, fades after hold even while the ability keeps animating
    Bar b;
    assert(!b.Update({1, true, 2.0f, 0, 0, 0, "Fireball"}, 0.0) && b.show && Near(b.progress, 0));
    b.Update({1, true, 1.0f}, 1.0);
    assert(Near(b.progress, 0.5f));
    b.Update({1, true, 0.0f}, 2.0);
    assert(Near(b.progress, 1) && b.readyAt == 2.0 && Near(b.Scale(2.0), kPunch) && Near(b.Flash(2.0), 1));
    assert(b.Visible(2.0 + kPunchDur + kHold) && !b.Visible(2.0 + kPunchDur + kHold + style::motion::kFadeOut.dur));
    assert(b.Update({1, false}, 3.0) && !b.cancelled && b.log == "Fireball cast 2.00s hold 0/0");
    assert(!b.Update({1, false}, 3.1));  // ends once

    // hold: 3 levels, 0.5 s each; level 0 shows nothing; full bar stays until release
    Bar h;
    h.Update({2, true, 0, 0, 3, 0.5f}, 0.0);
    assert(!h.show);
    h.Update({2, true, 0, 1, 3, 0.5f}, 0.5);
    h.Update({2, true, 0, 1, 3, 0.5f}, 0.75);
    assert(h.show && h.segs == 3 && Near(h.progress, 0.5f));
    h.Update({2, true, 0, 3, 3, 0.5f}, 1.5);
    assert(Near(h.progress, 1) && h.Visible(10.0));
    assert(h.Update({2, false}, 11.0) && !h.cancelled && !h.Visible(11.0 + style::motion::kFadeOut.dur));

    // cancelled: another ability starts before ready
    Bar c;
    c.Update({3, true, 2.0f}, 0.0);
    c.ability = "Salvo";
    assert(c.Update({4, true, 0}, 0.5) && c.id == 4 && !c.show && c.log == "Salvo cast 2.00s hold 0/0 cancelled");

    std::puts("ok");
}

// just test
#include "charge-bar.hpp"
#include <cassert>
#include <cmath>
#include <cstdio>

using namespace charge_bar;

static bool Near(float a, float b) { return std::fabs(a - b) < 1e-3f; }

int main() {
    // hold: 3 levels, 0.5 s each; level 0 shows nothing; full bar punches, stays until release
    Bar h;
    h.Update({2, true, 0, 3, 0.5f, "ShootArrow"}, 0.0);
    assert(!h.show);
    h.Update({2, true, 1, 3, 0.5f}, 0.5);
    h.Update({2, true, 1, 3, 0.5f}, 0.75);
    assert(h.show && h.segs == 3 && Near(h.progress, 0.5f));
    h.Update({2, true, 3, 3, 0.5f}, 1.5);
    assert(Near(h.progress, 1) && h.readyAt == 1.5 && Near(h.Scale(1.5), kPunch) && Near(h.Flash(1.5), 1) && h.Visible(10.0));
    assert(h.Update({2, false}, 11.0) && !h.cancelled && h.log == "ShootArrow hold 3/3");
    assert(!h.Visible(11.0 + style::motion::kFadeOut.dur) && !h.Update({2, false}, 11.1));  // ends once

    // a plain cast (no hold levels) shows nothing: its wind-up is cast-indicator's ring
    Bar p;
    assert(!p.Update({5, true, 0, 0, 0, "AimedShot"}, 0.0) && !p.show && !p.Update({5, false}, 1.0));

    // cancelled: another ability starts before full
    Bar c;
    c.Update({3, true, 1, 3, 0.5f, "ShootArrow"}, 0.0);
    assert(c.Update({4, true}, 0.2) && c.id == 4 && !c.show && c.log == "ShootArrow hold 1/3 cancelled");

    std::puts("ok");
}

// just test
#include "ow-movement.hpp"
#include <cassert>
#include <cmath>
#include <cstdio>

using namespace ow_movement;

static bool Eq(float a, float b) { return std::fabs(a - b) < 1e-3f; }

int main() {
    const Ground v{2048, 2048, 8, 2, 420};

    Ground g = Blend(v, 0, 1);  // off = vanilla
    assert(Eq(g.maxAccel, 2048) && Eq(g.brakingWalking, 2048) && Eq(g.groundFriction, 8) && Eq(g.jumpZ, 420));

    g = Blend(v, 1, 1);         // full snap
    assert(Eq(g.maxAccel, kSnapAccel) && Eq(g.brakingWalking, kSnapBraking) && Eq(g.groundFriction, kSnapFriction));
    assert(Eq(g.brakingFrictionFactor, 2));  // untouched
    assert(TimeTo(600, g.maxAccel) < 0.1f);

    g = Blend(v, 0.5f, 1);
    assert(Eq(g.maxAccel, (2048 + kSnapAccel) / 2));

    // A game that is already snappier than our target is never slowed down.
    const Ground fast{20000, 30000, 20, 2, 420};
    g = Blend(fast, 1, 1);
    assert(Eq(g.maxAccel, 20000) && Eq(g.brakingWalking, 30000) && Eq(g.groundFriction, 20));

    // Jump scale is clamped.
    assert(Eq(Blend(v, 0, 1.25f).jumpZ, 525) && Eq(Blend(v, 0, 9).jumpZ, 840) && Eq(Blend(v, 0, 0).jumpZ, 210));
    // Snap is clamped.
    assert(Eq(Blend(v, 7, 1).maxAccel, kSnapAccel) && Eq(Blend(v, -1, 1).maxAccel, 2048));
    std::puts("ok");
}

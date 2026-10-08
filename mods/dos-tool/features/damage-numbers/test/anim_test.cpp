// just test
#include "anim.hpp"
#include <cassert>
#include <cmath>
#include <algorithm>
#include <cstdio>

int main() {
    auto near = [](float a, float b) { return std::fabs(a - b) < 0.5f; };
    // animation curve
    auto A = [](double t, float big) { return dmgnum::Animate(t, t, 1.4, 1, big, false); };
    auto a0 = A(0.0, 0), a1 = A(0.12, 0), a2 = A(0.5, 0), a3 = A(1.4, 0);
    assert(a0.scale == 0 && a0.flash == 1 && a0.alpha == 1);           // starts from nothing, white-hot
    assert(a1.scale > 1.0f);                                             // overshoot during pop
    assert(near(a2.scale, 1.0f) && a2.alpha == 1 && a2.dy < -1.0f);      // settled, risen, opaque
    assert(a3.alpha == 0 && a3.scale < 1.0f);                            // gone at end of life
    float peakTypical = 0, peakBig = 0;
    for (double t = 0; t < 0.25; t += 0.005) {
        peakTypical = std::max(peakTypical, A(t, 0).scale);
        peakBig = std::max(peakBig, A(t, 1).scale);
    }
    assert(peakBig > peakTypical);                                       // big hits punch harder
    // stacked number: bump kicks scale + flash, stays alive while hits keep coming
    auto settled = dmgnum::Animate(2.0, 0.6, 1.4, 0, 0, true), kicked = dmgnum::Animate(2.0, 0.0, 1.4, 0, 0, true);
    assert(kicked.scale > settled.scale + 0.3f && kicked.flash == 1 && kicked.alpha == 1);
    std::puts("ok");
}

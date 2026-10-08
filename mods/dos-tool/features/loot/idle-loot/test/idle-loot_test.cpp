// just test
#include "idle-loot.hpp"
#include <cassert>
#include <cmath>
#include <cstdio>

int main() {
    using namespace idle_loot;
    // breathing stays in 0.75..1.0
    for (double t = 0; t < 5; t += 0.01) {
        const float b = Breathe(t, 0.3f);
        assert(b >= 0.749f && b <= 1.001f);
    }
    // each glint: alive kGlintLife per kShimmerPeriod, peaks at 1, inside its area; neighbours out of sync
    for (int k = 0; k < kGlints; k++) {
        float peak = 0, alive = 0;
        const double dt = 0.001;
        for (double t = 10; t < 10 + style::motion::kShimmerPeriod; t += dt) {
            const Glint g = GlintAt(0xABCDEF10ull, t, k);
            assert(g.scale >= 0 && g.scale <= 1.0001f);
            if (g.scale > 0) {
                alive += float(dt);
                assert(std::fabs(g.dx) <= kAreaRx && g.dy >= -kAreaUp && g.dy <= kAreaDown);
            }
            peak = std::max(peak, g.scale);
        }
        assert(peak > 0.99f && std::fabs(alive - kGlintLife) < 0.01f);
    }
    assert(Phase(0x1000) != Phase(0x1008));
    // distance: full until 25 m, gone at 30 m; depth clamped
    assert(DistFade(1000) == 1 && DistFade(kCull) == 0 && DistFade(2750) > 0.4f && DistFade(2750) < 0.6f);
    assert(Depth(100) == 1.25f && Depth(10000) == 0.6f);
    // looted: alpha reaches 0 within kFadeOut (<= 1 s); fade in over kFadeIn
    float a = 1;
    for (int i = 0; i < 60 && a > 0; i++) a = Approach(a, 0, 1 / 60.f);
    assert(a == 0 && style::motion::kFadeOut.dur <= 1);
    a = Approach(0, 1, style::motion::kFadeIn.dur);
    assert(a == 1);
    // occlusion: rendered recently = on screen; unknown = on screen
    assert(OnScreen(9.95f, 10) && !OnScreen(9.5f, 10) && OnScreen(NAN, 10) && OnScreen(-1000, 0));
    std::puts("idle-loot ok");
}

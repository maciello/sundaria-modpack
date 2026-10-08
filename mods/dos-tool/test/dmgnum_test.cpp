// just test
#include "dmgnum.hpp"
#include <cassert>
#include <cmath>
#include <algorithm>
#include <cstdio>

using dmgnum::Kind;

int main() {
    dmgnum::Tracker t;
    // spawn: 0 -> full HP inside the grace window is not a heal
    t.Update({{1, 0, 0, 0, 0, false}}, 0.0);
    t.Update({{1, 0, 0, 0, 100, false}}, 0.2);
    assert(t.live.empty());
    // HP set from 0 after grace (late init) is not a heal either
    t.Update({{1, 0, 0, 0, 100, false}, {2, 0, 0, 0, 0, false}}, 0.3);
    t.Update({{1, 0, 0, 0, 100, false}, {2, 0, 0, 0, 80, false}}, 2.0);
    assert(t.live.empty());

    t.Update({{1, 0, 0, 0, 70, false}, {2, 0, 0, 0, 80, false}}, 2.1);
    assert(t.live.size() == 1 && t.live[0].kind == Kind::Dealt && t.live[0].amount == 30);
    assert(std::fabs(t.live[0].scale - 1.0f) < 1e-3);                   // first hit defines "typical"
    t.Update({{1, 0, 0, 0, 80, false}, {2, 0, 0, 0, 80, false}}, 2.2);
    assert(t.live.back().kind == Kind::Heal && t.live.back().amount == 10);
    t.Update({{1, 0, 0, 0, 80, false}, {2, 0, 0, 0, 80, false}, {3, 0, 0, 0, 500, true}}, 2.3);
    t.Update({{1, 0, 0, 0, 80, false}, {2, 0, 0, 0, 80, false}, {3, 0, 0, 0, 450, true}}, 4.0);
    assert(t.live.back().kind == Kind::Taken && t.fight.total == 30);    // damage taken is not DPS

    // relative size: 4x typical is bigger, tiny is smaller, clamped
    dmgnum::Tracker s;
    assert(s.Scale(20) == 1.0f);
    assert(s.Scale(80) > 1.5f && s.Scale(1) < 0.8f && s.Scale(1e9f) <= 2.2f);
    // late game: typical drifts up, so the same 400 shrinks
    dmgnum::Tracker late;
    late.Scale(400);
    for (int i = 0; i < 100; i++) late.Scale(20000);
    assert(late.Scale(400) < 0.7f);

    // fights: total/DPS, new fight after the gap
    dmgnum::Tracker f;
    f.Update({{9, 0, 0, 0, 1000, false}}, 0.0);
    f.Update({{9, 0, 0, 0, 900, false}}, 2.0);
    f.Update({{9, 0, 0, 0, 700, false}}, 4.0);
    assert(f.fight.total == 300 && f.fight.Dps() == 150 && f.FightActive(4.0));
    assert(!f.FightActive(9.5));
    f.Update({{9, 0, 0, 0, 650, false}}, 10.0);
    assert(f.fight.total == 50);

    // despawn + id reuse: treated as new
    dmgnum::Tracker r;
    r.Update({{1, 0, 0, 0, 100, false}}, 0.0);
    r.Update({}, 3.0);
    r.Update({{1, 0, 0, 0, 5, false}}, 3.1);
    assert(r.live.empty());
    r.Update({}, 10.0);
    assert(r.live.empty());

    // projection
    auto near = [](float a, float b) { return std::fabs(a - b) < 0.5f; };
    float sx, sy;
    dmgnum::View v{0, 0, 0, 0, 0, 0, 90};
    assert(dmgnum::Project(v, 100, 0, 0, 1920, 1080, sx, sy) && near(sx, 960) && near(sy, 540));
    assert(dmgnum::Project(v, 100, 100, 0, 1920, 1080, sx, sy) && near(sx, 1920) && near(sy, 540));
    assert(dmgnum::Project(v, 100, 0, 50, 1920, 1080, sx, sy) && near(sx, 960) && near(sy, 60));
    assert(!dmgnum::Project(v, -100, 0, 0, 1920, 1080, sx, sy));
    dmgnum::View turned{0, 0, 0, 0, 90, 0, 90};
    assert(dmgnum::Project(turned, 0, 100, 0, 1920, 1080, sx, sy) && near(sx, 960));
    dmgnum::View down{0, 0, 0, -45, 0, 0, 90};
    assert(dmgnum::Project(down, 100, 0, -100, 1920, 1080, sx, sy) && near(sx, 960) && near(sy, 540));

    // animation curve
    auto a0 = dmgnum::Animate(0.0, 1.4, 1, 0), a1 = dmgnum::Animate(0.12, 1.4, 1, 0);
    auto a2 = dmgnum::Animate(0.5, 1.4, 1, 0), a3 = dmgnum::Animate(1.4, 1.4, 1, 0);
    assert(a0.scale == 0 && a0.flash == 1 && a0.alpha == 1);           // starts from nothing, white-hot
    assert(a1.scale > 1.0f);                                             // overshoot during pop
    assert(near(a2.scale, 1.0f) && a2.alpha == 1 && a2.dy < -1.0f);      // settled, risen, opaque
    assert(a3.alpha == 0 && a3.scale < 1.0f);                            // gone at end of life
    float peakTypical = 0, peakBig = 0;
    for (double t = 0; t < 0.25; t += 0.005) {
        peakTypical = std::max(peakTypical, dmgnum::Animate(t, 1.4, 0, 0).scale);
        peakBig = std::max(peakBig, dmgnum::Animate(t, 1.4, 0, 1).scale);
    }
    assert(peakBig > peakTypical);                                       // big hits punch harder
    std::puts("ok");
}

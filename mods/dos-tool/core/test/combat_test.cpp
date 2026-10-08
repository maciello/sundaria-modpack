// just test
#include "combat.hpp"
#include <cassert>
#include <cmath>
#include <algorithm>
#include <cstdio>

using combat::Kind;

int main() {
    combat::Tracker t;
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
    combat::Tracker s;
    assert(s.Scale(20) == 1.0f);
    assert(s.Scale(80) > 1.5f && s.Scale(1) == 0.8f && s.Scale(1e9f) <= 2.4f);   // floor 0.8: small hits stay readable
    // late game: typical drifts up, so the same 400 shrinks
    combat::Tracker late;
    late.Scale(400);
    for (int i = 0; i < 100; i++) late.Scale(20000);
    assert(late.Scale(400) == 0.8f);

    // fights: total/DPS, new fight after the gap
    combat::Tracker f;
    f.Update({{9, 0, 0, 0, 1000, false}}, 0.0);
    f.Update({{9, 0, 0, 0, 900, false}}, 2.0);
    f.Update({{9, 0, 0, 0, 700, false}}, 4.0);
    assert(f.fight.total == 300 && f.fight.Dps() == 150 && f.FightActive(4.0));
    assert(!f.FightActive(9.5));
    f.Update({{9, 0, 0, 0, 650, false}}, 10.0);
    assert(f.fight.total == 50);

    // despawn + id reuse: treated as new
    combat::Tracker r;
    r.Update({{1, 0, 0, 0, 100, false}}, 0.0);
    r.Update({}, 3.0);
    r.Update({{1, 0, 0, 0, 5, false}}, 3.1);
    assert(r.live.empty());
    r.Update({}, 10.0);
    assert(r.live.empty());

    // projection
    auto near = [](float a, float b) { return std::fabs(a - b) < 0.5f; };
    float sx, sy;
    combat::View v{0, 0, 0, 0, 0, 0, 90};
    assert(combat::Project(v, 100, 0, 0, 1920, 1080, sx, sy) && near(sx, 960) && near(sy, 540));
    assert(combat::Project(v, 100, 100, 0, 1920, 1080, sx, sy) && near(sx, 1920) && near(sy, 540));
    assert(combat::Project(v, 100, 0, 50, 1920, 1080, sx, sy) && near(sx, 960) && near(sy, 60));
    assert(!combat::Project(v, -100, 0, 0, 1920, 1080, sx, sy));
    combat::View turned{0, 0, 0, 0, 90, 0, 90};
    assert(combat::Project(turned, 0, 100, 0, 1920, 1080, sx, sy) && near(sx, 960));
    combat::View down{0, 0, 0, -45, 0, 0, 90};
    assert(combat::Project(down, 100, 0, -100, 1920, 1080, sx, sy) && near(sx, 960) && near(sy, 540));


    // stacking: same target within window merges, total grows, size grows
    combat::Tracker st;
    st.Update({{5, 0, 0, 0, 1000, false}, {6, 0, 0, 0, 1000, false}}, 0.0);
    st.Update({{5, 0, 0, 0, 980, false}, {6, 0, 0, 0, 1000, false}}, 2.0);
    const float s1 = st.live[0].scale;
    st.Update({{5, 0, 0, 0, 960, false}, {6, 0, 0, 0, 1000, false}}, 2.5);
    st.Update({{5, 0, 0, 0, 940, false}, {6, 0, 0, 0, 990, false}}, 3.0);   // other target: separate
    assert(st.live.size() == 2 && st.live[0].amount == 60 && st.live[0].hits == 3 && st.live[0].scale > s1);
    assert(std::fabs(st.typical - 19.2f) < 0.01f && st.live[1].amount == 10); // typical learns single hits (20,20,20,10), not the 60 stack
    st.Update({{5, 0, 0, 0, 920, false}, {6, 0, 0, 0, 990, false}}, 4.2);  // 1.2 s gap > stack window: new number, old still fading
    assert(st.live.size() == 3 && st.live[2].amount == 20);
    st.Update({{5, 0, 0, 0, 920, false}, {6, 0, 0, 0, 990, false}}, 4.2 + 1.5);
    assert(st.live.empty());                                               // all expired after last bump
    std::puts("ok");
}

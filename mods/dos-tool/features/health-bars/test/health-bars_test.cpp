// just test
#include "health-bars.hpp"
#include <cassert>
#include <cstdio>

int main() {
    health_bars::Bars b;
    combat::Sample e{1, 0, 0, 0, 100, false, 100, 12}, p{2, 0, 0, 0, 50, true, 100};
    assert(b.Update({e, p}, 0.0, 0.016).empty());                  // full-HP enemy, player: no bar
    e.health = 60;
    auto v = b.Update({e, p}, 1.0, 0.016);
    assert(v.size() == 1 && v[0].frac == 0.6f && v[0].chip == 1.0f); // hit: chip holds
    assert(v[0].alpha == 0.0f && v[0].flash == 1.0f && v[0].level == 12); // fades in from 0, flashes
    v = b.Update({e}, 1.3, 0.3);
    assert(v[0].chip == 1.0f && v[0].alpha == 1.0f && v[0].flash == 0.0f);
    v = b.Update({e}, 1.8, 0.25);
    assert(v[0].chip < 1.0f && v[0].chip > 0.6f);                   // draining
    v = b.Update({e}, 3.0, 1.0);
    assert(v[0].chip == 0.6f);                                      // caught up, never below fill
    // healed to full: lingers, then fades out, then gone
    e.health = 100;
    assert(b.Update({e}, 4.0, 0.016).size() == 1);
    assert(b.Update({e}, 6.9, 0.016)[0].alpha == 1.0f);             // still within linger (3 s)
    assert(b.Update({e}, 7.2, 0.016)[0].alpha == 1.0f);             // linger over: fade-out starts
    v = b.Update({e}, 7.4, 0.016);
    assert(v.size() == 1 && v[0].alpha < 1.0f && v[0].alpha > 0.0f); // fading out
    assert(b.Update({e}, 7.7, 0.016).empty());                      // gone
    // max attribute above current HP on an unhit enemy: no bar
    health_bars::Bars u;
    combat::Sample big{4, 0, 0, 0, 100, false, 150};
    assert(u.Update({big}, 0.0, 0.016).empty() && u.Update({big}, 5.0, 0.016).empty());
    // hit enemy behind a wall (mesh not on screen for > occludedAfter): hidden, back when visible
    health_bars::Bars o;
    combat::Sample w{5, 0, 0, 0, 100, false}, me{6, 0, 0, 0, 100, true};
    w.seen = me.seen = 10.0f;
    o.Update({w, me}, 0.0, 0.016);
    w.health = 50;
    assert(o.Update({w, me}, 1.0, 0.016).size() == 1);
    me.seen = 11.0f;
    assert(o.Update({w, me}, 2.0, 0.016).empty());
    w.seen = 11.0f;
    assert(o.Update({w, me}, 2.1, 0.016).size() == 1);
    // death fades out instead of vanishing
    health_bars::Bars d;
    combat::Sample m{3, 0, 0, 0, 200, false, 0};
    d.Update({m}, 0.0, 0.016);
    m.health = 50;
    assert(d.Update({m}, 1.0, 0.016)[0].frac == 0.25f);            // max = peak HP seen
    m.health = 0;
    v = d.Update({m}, 2.0, 0.016);
    assert(v.size() == 1 && v[0].frac == 0.0f);
    assert(d.Update({m}, 2.5, 0.016).empty());
    assert(d.Update({}, 3.0, 0.016).empty() && d.st.empty());       // despawn forgets state
    std::puts("ok");
}

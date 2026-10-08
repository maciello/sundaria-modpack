// just test
#include "health-bars.hpp"
#include <cassert>
#include <cstdio>

int main() {
    health_bars::Bars b;
    combat::Sample e{1, 0, 0, 0, 100, false, 100}, p{2, 0, 0, 0, 50, true, 100};
    assert(b.Update({e, p}, 0.0, 0.016).empty());                  // full HP enemy, player: no bar
    e.health = 60;
    auto v = b.Update({e, p}, 1.0, 0.016);
    assert(v.size() == 1 && v[0].frac == 0.6f && v[0].chip == 1.0f); // hit: chip holds
    v = b.Update({e}, 1.3, 0.3);
    assert(v[0].chip == 1.0f);                                      // within chipDelay
    v = b.Update({e}, 1.8, 0.25);
    assert(v[0].chip < 1.0f && v[0].chip > 0.6f);                   // draining
    v = b.Update({e}, 3.0, 1.0);
    assert(v[0].chip == 0.6f);                                      // caught up, never below fill
    // unknown max attribute: peak HP seen is the max
    health_bars::Bars u;
    combat::Sample m{3, 0, 0, 0, 200, false, 0};
    u.Update({m}, 0.0, 0.016);
    m.health = 50;
    assert(u.Update({m}, 1.0, 0.016)[0].frac == 0.25f);
    // dead: no bar; despawn forgets state
    m.health = 0;
    assert(u.Update({m}, 2.0, 0.016).empty());
    assert(u.Update({}, 3.0, 0.016).empty() && u.st.empty());
    std::puts("ok");
}

// c++ -std=c++20 -Isrc test/dmgnum_test.cpp -o /tmp/dmgnum_test && /tmp/dmgnum_test
#include "dmgnum.hpp"
#include <cassert>
#include <cstdio>

int main() {
    dmgnum::Tracker t;
    t.Update({{1, 0, 0, 0, 100}}, 0.0);
    assert(t.live.empty());                                  // first sight: no number
    t.Update({{1, 0, 0, 0, 70}, {2, 0, 0, 0, 50}}, 0.1);
    assert(t.live.size() == 1 && t.live[0].amount == 30);    // 30 damage; new actor 2 silent
    t.Update({{1, 0, 0, 0, 80}, {2, 0, 0, 0, 50}}, 0.2);
    assert(t.live.size() == 2 && t.live[1].amount == -10);   // heal shows negative
    t.Update({{2, 0, 0, 0, 50}}, 0.3);                       // actor 1 despawns
    t.Update({{1, 0, 0, 0, 5}}, 0.4);                        // id reused: treated as new, no bogus 75
    assert(t.live.size() == 2);
    t.Update({}, 2.0);
    assert(t.live.empty());                                  // expired after lifetime
    std::puts("ok");
}

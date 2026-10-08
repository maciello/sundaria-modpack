// c++ -std=c++20 -Isrc test/dmgnum_test.cpp -o /tmp/dmgnum_test && /tmp/dmgnum_test
#include "dmgnum.hpp"
#include <cassert>
#include <cmath>
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

    auto near = [](float a, float b) { return std::fabs(a - b) < 0.5f; };
    float sx, sy;
    dmgnum::View v{0, 0, 0, 0, 0, 0, 90};
    assert(dmgnum::Project(v, 100, 0, 0, 1920, 1080, sx, sy) && near(sx, 960) && near(sy, 540));    // dead ahead
    assert(dmgnum::Project(v, 100, 100, 0, 1920, 1080, sx, sy) && near(sx, 1920) && near(sy, 540)); // 45deg right = edge (90 FOV)
    assert(dmgnum::Project(v, 100, 0, 50, 1920, 1080, sx, sy) && near(sx, 960) && near(sy, 60));    // up = smaller y
    assert(!dmgnum::Project(v, -100, 0, 0, 1920, 1080, sx, sy));                                     // behind
    dmgnum::View turned{0, 0, 0, 0, 90, 0, 90};                                                       // yaw 90: looking +Y
    assert(dmgnum::Project(turned, 0, 100, 0, 1920, 1080, sx, sy) && near(sx, 960));
    dmgnum::View down{0, 0, 0, -45, 0, 0, 90};                                                        // pitched down 45
    assert(dmgnum::Project(down, 100, 0, -100, 1920, 1080, sx, sy) && near(sx, 960) && near(sy, 540));
    std::puts("ok");
}

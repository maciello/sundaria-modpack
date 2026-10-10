// just test
#include "../dual-wield-damage.hpp"
#include <cassert>
#include <cmath>
#include <cstdio>

using namespace dual_wield_damage;
static bool Near(float a, float b) { return std::fabs(a - b) < 1e-4f; }

int main() {
    // two equal weapons: each cast x1.85
    Out o = Plan({true, true, false, 100, 100}, 0.85f);
    assert(o.apply && Near(o.l, 185) && Near(o.r, 185));
    // unequal: own + k * other, per hand
    o = Plan({true, true, false, 100, 60}, 0.85f);
    assert(o.apply && Near(o.l, 151) && Near(o.r, 145));
    // 2H weapon (both hands one set), shield / empty hand (0 or missing), k = 0: vanilla, nothing applied
    assert(!Plan({true, true, true, 190, 190}, 0.85f).apply);
    assert(!Plan({true, true, false, 100, 0}, 0.85f).apply);
    assert(!Plan({true, false, false, 100, 0}, 0.85f).apply);
    o = Plan({true, true, false, 100, 60}, 0.0f);
    assert(!o.apply && Near(o.l, 100) && Near(o.r, 60));  // restore value = vanilla
    // primary ability: vanilla = boosted x factor, per hand (1/1.85 for equal weapons)
    assert(Near(PrimaryFactor(100, 100, 0.85f), 1.0f / 1.85f));
    o = Plan({true, true, false, 100, 60}, 0.85f);
    assert(Near(o.l * PrimaryFactor(100, 60, 0.85f), 100) && Near(o.r * PrimaryFactor(60, 100, 0.85f), 60));
    assert(Near(PrimaryFactor(100, 60, 0.0f), 1.0f) && Near(PrimaryFactor(0, 5, 0.85f), 1.0f));
    std::puts("ok");
}

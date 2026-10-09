// just test
#include "capture.hpp"
#include <cassert>
#include <cstdio>

using namespace dps_probe;

int main() {
    Keep k;
    assert(k.NewClass(7) && k.Take(7, false) && !k.NewClass(7));
    assert(!k.Take(7, false));                    // second plain hit: nothing new
    assert(k.Take(7, true) && !k.Take(7, true));  // first crit, once
    assert(k.Take(8, true) && k.Take(8, false));  // crit first: the plain hit is still wanted
    for (uint64_t c = 100; k.seen.size() < size_t(Keep::kMaxClasses); c++) assert(k.Take(c, false));
    assert(!k.Take(999, false) && !k.Take(999, true));  // cap: no new classes
    assert(k.Take(100, true));                           // known classes still get their crit
    std::puts("dps-probe ok");
}

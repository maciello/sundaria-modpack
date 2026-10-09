// native: just test
#include "cost.hpp"
#include <cassert>
#include <cstdio>

int main() {
    cost::Path p{"x"};
    assert(p.Record(0.0));            // first call logs even at 0 ms
    assert(p.Line(0.0).find("first") != std::string::npos);
    assert(!p.Record(0.0));           // equal: no log
    assert(p.Record(0.5));            // new max
    assert(p.Line(0.5).find("max, call 3") != std::string::npos);
    assert(!p.Record(0.4));
    cost::Avg a{"d", 3};
    assert(!a.Add(0.001) && !a.Add(0.002) && a.Add(0.003));
    assert(a.Line().find("avg 2.00 us, max 3.0 us over 3 samples") != std::string::npos);
    assert(a.n == 0 && !a.Add(0.001));  // reset
    std::puts("ok");
}

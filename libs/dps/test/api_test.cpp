// just test: the public API compiles SDK-free and links.
#include "dps/dps.hpp"
#include <cassert>
#include <cstdio>

int main() {
    dps::Model m{dps::Tables{}};
    dps::Build b;
    b.cls = "Ranger";
    assert(!m.Dps(b, dps::Scenario{}).error.empty());
    std::puts("ok");
}

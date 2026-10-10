// just test
#include "../xp-by-map-level.hpp"
#include <cassert>
#include <cmath>
#include <cstdio>

using namespace xp_by_map_level;

int main() {
    const Curve k;
    // issue #134 table (+-0.02); d=75: the issue says 4.1, its own formula gives 4.144
    const struct { int d; double m; } t[] = {{-50, 0.05}, {-20, 0.24}, {-10, 0.68}, {-5, 0.88}, {-3, 0.94}, {0, 1.0}, {3, 1.02},
                                              {10, 1.09}, {20, 1.26}, {30, 1.55}, {50, 2.61}, {75, 4.144}, {100, 4.8}};
    for (auto& r : t) {
        std::printf("d=%d m=%.3f want %.2f\n", r.d, Mult(r.d, k), r.m);
        assert(std::fabs(Mult(r.d, k) - r.m) <= 0.02);
    }
    assert(std::fabs(Mult(100000, k) - 5.0) < 1e-3);
    assert(Extra(100, 1.0) == 0 && Extra(100, 0.5) == 0 && Extra(0, 3.0) == 0);
    assert(Extra(100, 2.5) == 150 && Extra(7, 1.02) == 0);
    std::puts("ok");
}

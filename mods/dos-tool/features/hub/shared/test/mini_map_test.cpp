// just test
#include "../mini_map_math.hpp"
#include <cassert>
#include <cmath>
#include <cstdio>

static bool Near(float a, float b) { return std::fabs(a - b) < 0.01f; }

int main() {
    using namespace mini_map;
    // centre lands on the desk, at the desk's height
    Xf c = Shrink({1000, 2000, 500, 10, 2}, 1000, 2000, 500, 50, 60, 90, 0, 0.1f);
    assert(Near(c.x, 50) && Near(c.y, 60) && Near(c.z, 90) && Near(c.yaw, 10) && Near(c.scale, 0.2f));
    // offsets shrink by k
    Xf a = Shrink({1100, 2000, 600, 0, 1}, 1000, 2000, 500, 0, 0, 0, 0, 0.1f);
    assert(Near(a.x, 10) && Near(a.y, 0) && Near(a.z, 10));
    // desk turned 90°: +X offset becomes +Y, yaw adds
    Xf b = Shrink({1100, 2000, 500, 30, 1}, 1000, 2000, 500, 0, 0, 0, 90, 0.1f);
    assert(Near(b.x, 0) && Near(b.y, 10) && Near(b.yaw, 120));
    std::puts("ok");
}

// just test
#include "../aim.hpp"
#include <cassert>
#include <cmath>
#include <cstdio>

static bool Near(float a, float b) { return std::fabs(a - b) < 0.01f; }

int main() {
    using namespace hub_aim;
    // looked-at target: straight ahead wins, behind / too far / outside the cone loses
    std::vector<Target> ts{{1000, 300, 0}, {1000, 10, 0}, {-500, 0, 0}, {5000, 0, 0}};
    assert(LookedAt(0, 0, 0, 0, 0, ts, 10, 2000, 0) == 1);
    assert(LookedAt(0, 0, 0, 0, 180, ts, 10, 2000, 0) == 2);       // turned around
    assert(LookedAt(0, 0, 0, 0, 90, ts, 10, 2000, 0) == -1);       // nothing to the right
    assert(LookedAt(0, 0, 0, 0, 0, {{5000, 0, 0}}, 10, 2000, 0) == -1);  // too far
    assert(LookedAt(0, 0, 300, -16.7f, 0, {{1000, 0, 0}}, 3, 2000, 0) == 0);  // looking down at it
        // view ray to the floor: 45° down from 100 above → 100 ahead; level or upward view or too far → none
    float fx, fy;
    assert(RayToFloor(0, 0, 100, -45, 0, 0, 1000, fx, fy) && Near(fx, 100) && Near(fy, 0));
    assert(RayToFloor(0, 0, 100, -45, 90, 0, 1000, fx, fy) && Near(fx, 0) && Near(fy, 100));
    assert(!RayToFloor(0, 0, 100, 0, 0, 0, 1000, fx, fy) && !RayToFloor(0, 0, 100, 10, 0, 0, 1000, fx, fy));
    assert(!RayToFloor(0, 0, 100, -1.5f, 0, 0, 1000, fx, fy));      // hits 3.8 km away
    assert(!RayToFloor(0, 0, -5, -45, 0, 0, 1000, fx, fy));         // camera below that floor
        // ray pick: the nearer of two spheres on the ray wins; a miss and one behind the camera don't count
    std::vector<Sphere> ss{{500, 0, 0, 50}, {200, 0, 0, 30}, {300, 200, 0, 50}, {-300, 0, 0, 80}};
    assert(RayPick(0, 0, 0, 0, 0, ss, 2000) == 1);
    assert(RayPick(0, 0, 0, 0, 0, {{500, 40, 0, 50}}, 2000) == 0);   // grazes it
    assert(RayPick(0, 0, 0, 0, 0, {{500, 60, 0, 50}}, 2000) == -1);  // misses
    assert(RayPick(0, 0, 0, 0, 180, ss, 2000) == 3);                 // turned around
    assert(RayPick(0, 0, 0, 0, 0, {{5000, 0, 0, 50}}, 2000) == -1);  // too far
        std::puts("ok");
}

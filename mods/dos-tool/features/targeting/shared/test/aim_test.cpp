// just test
#include "aim.hpp"
#include <cassert>
#include <cmath>
#include <cstdio>

using namespace aim;

static bool Near(V3 a, V3 b, float eps = 0.01f) { return Len(a - b) < eps; }

int main() {
    assert(Near(Dir(0, 0), {1, 0, 0}) && Near(Dir(0, 90), {0, 1, 0}) && Near(Dir(90, 0), {0, 0, 1}));

    // camera 400 behind and 100 above the hero, looking along +X: the ray leaves the 1000 sphere around the hero
    const V3 cam{-400, 0, 100}, hero{0, 0, 0}, fwd{1, 0, 0};
    V3 out{};
    assert(ClipCameraRay(cam, fwd, hero, 1000, out));
    assert(std::fabs(out.x - std::sqrt(1000.f * 1000 - 100 * 100)) < 0.1f && out.z == 100);  // 400 to the closest point, then the half chord
    assert(!ClipCameraRay(cam, V3{-1, 0, 0}, hero, 1000, out));          // looking away from the hero
    assert(!ClipCameraRay({-400, 0, 5000}, fwd, hero, 1000, out));       // ray passes above the sphere

    // no line-trace hit: sweep from the hero towards the clipped view end, exactly `range` long
    V3 e = SweepEnd(cam, fwd, hero, 1000, false, {});
    assert(std::fabs(Len(e - hero) - 1000) < 0.1f && e.z > 0);           // pitched up towards the camera-height point
    // a hit within range: aim at it
    e = SweepEnd(cam, fwd, hero, 1000, true, {500, 0, 0});
    assert(Near(e, {1000, 0, 0}));
    // a hit beyond range is ignored (view end used)
    assert(Near(SweepEnd(cam, fwd, hero, 1000, true, {5000, 0, 0}), SweepEnd(cam, fwd, hero, 1000, false, {})));

    assert(Targeted(2, false) && Targeted(1, true) && !Targeted(1, false) && !Targeted(0, true) && !Targeted(3, true));
    assert(Mark(2, true, false, false, true) == Kind::Ally);
    assert(Mark(2, true, false, true, true) == Kind::None);              // heal swept an enemy first: no target
    assert(Mark(1, true, false, true, true) == Kind::Enemy);
    assert(Mark(1, true, false, false, true) == Kind::None);
    assert(Mark(2, true, true, false, true) == Kind::None);              // self
    assert(Mark(1, true, false, true, false) == Kind::None);             // dead
    assert(Mark(2, false, false, false, true) == Kind::None);
    std::puts("ok");
}

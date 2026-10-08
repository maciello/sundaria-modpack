// just test
#include "free-camera.hpp"
#include <cassert>
#include <cmath>
#include <cstdio>

static bool Near(float a, float b) { return std::fabs(a - b) < 0.01f; }

int main() {
    using namespace free_camera;
    const Pose o{0, 0, 0, 0, 0};
    Pose p = Step(o, {1, 0, 0, 0, 0, false}, 1.0f, 100.0f);
    assert(Near(p.x, 100) && Near(p.y, 0) && Near(p.z, 0));     // forward = +X at yaw 0
    p = Step(o, {0, 1, 0, 0, 0, false}, 1.0f, 100.0f);
    assert(Near(p.x, 0) && Near(p.y, 100));                      // right = +Y
    p = Step({0, 0, 0, 0, 90}, {1, 0, 0, 0, 0, false}, 1.0f, 100.0f);
    assert(Near(p.x, 0) && Near(p.y, 100));                      // yaw 90: forward = +Y
    p = Step({0, 0, 0, 0, 90}, {0, 1, 0, 0, 0, false}, 1.0f, 100.0f);
    assert(Near(p.x, -100) && Near(p.y, 0));                     // yaw 90: right = -X
    p = Step({0, 0, 0, 45, 0}, {1, 0, 0, 0, 0, false}, 1.0f, 100.0f);
    assert(Near(p.x, 70.71f) && Near(p.z, 70.71f));              // pitch climbs along the view
    p = Step({0, 0, 0, 45, 0}, {0, 1, 0, 0, 0, false}, 1.0f, 100.0f);
    assert(Near(p.z, 0));                                        // strafe stays flat
    p = Step(o, {0, 0, 1, 0, 0, true}, 0.5f, 100.0f);
    assert(Near(p.z, 200));                                      // fast = x4, dt scales
    p = Step(o, {0, 0, 0, 120, 30, false}, 1.0f, 100.0f);
    assert(Near(p.pitch, 89) && Near(p.yaw, 30));                // pitch clamps
    p = Step({0, 0, 0, -80, 0}, {0, 0, 0, -30, 0, false}, 1.0f, 100.0f);
    assert(Near(p.pitch, -89));
    std::puts("ok");
}

// just test
#include "tavern-hub.hpp"
#include <cassert>
#include <cmath>
#include <cstdio>

static bool Near(float a, float b) { return std::fabs(a - b) < 0.01f; }

int main() {
    using namespace tavern_hub;
    // camera at yaw 0, level: straight behind (-X), at head height
    Pose c = Follow(100, 0, 0, 0, 0, 300, 60);
    assert(Near(c.x, -200) && Near(c.y, 0) && Near(c.z, 60) && Near(c.yaw, 0));
    // looking down 30 degrees: raised by dist*sin(30)
    c = Follow(0, 0, 0, 90, -30, 200, 0);
    assert(Near(c.x, 0) && Near(c.y, -173.21f) && Near(c.z, 100) && Near(c.pitch, -30));

    float x, y;
    MoveDir(1, 0, 0, x, y);  assert(Near(x, 1) && Near(y, 0));
    MoveDir(1, 0, 90, x, y); assert(Near(x, 0) && Near(y, 1));
    MoveDir(0, 1, 90, x, y); assert(Near(x, -1) && Near(y, 0));
    MoveDir(1, 1, 0, x, y);  assert(Near(x * x + y * y, 1));   // diagonal not faster
    MoveDir(0, 0, 45, x, y); assert(x == 0 && y == 0);

    assert(Near(FaceYaw(0, 0, 0, 10), 90) && Near(FaceYaw(0, 0, -10, 0), 180));

    auto spots = ParseLayout("# c\nInn_Leena=1.5,2,3,90\r\nbad line\nGuard=4,5,6,-45\nInn_Leena=7,8,9,0\n=1,2,3,4\nX=1,2\n");
    assert(spots.size() == 2 && spots[0].name == "Inn_Leena" && Near(spots[0].x, 7) && spots[1].name == "Guard");
    SetSpot(spots, {"Guard", 0, 0, 0, 10});
    SetSpot(spots, {"Blacksmith_Kadrick", 1, 1, 1, 1});
    assert(spots.size() == 3 && Near(spots[1].yaw, 10));
    auto again = ParseLayout(WriteLayout(spots));
    assert(again.size() == 3 && again[2].name == "Blacksmith_Kadrick" && Near(again[0].z, 9));
    // looked-at target: straight ahead wins, behind / too far / outside the cone loses
    std::vector<Target> ts{{1000, 300, 0}, {1000, 10, 0}, {-500, 0, 0}, {5000, 0, 0}};
    assert(LookedAt(0, 0, 0, 0, 0, ts, 10, 2000, 0) == 1);
    assert(LookedAt(0, 0, 0, 0, 180, ts, 10, 2000, 0) == 2);       // turned around
    assert(LookedAt(0, 0, 0, 0, 90, ts, 10, 2000, 0) == -1);       // nothing to the right
    assert(LookedAt(0, 0, 0, 0, 0, {{5000, 0, 0}}, 10, 2000, 0) == -1);  // too far
    assert(LookedAt(0, 0, 300, -16.7f, 0, {{1000, 0, 0}}, 3, 2000, 0) == 0);  // looking down at it
        std::puts("ok");
}

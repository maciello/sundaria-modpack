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
    // scene record round-trips; junk lines are skipped; no centre = not a scene
    Scene sc;
    sc.cx = 1; sc.cy = 2; sc.cz = 3;
    sc.actors = {{"Button_Map_LichCastle", 10, 20, 30, 0, 90, 0, 7.25f, true}, {"wm_worldMap_2", -1, -2, -3, 1, 2, 3, 0.004f, false}};
    Scene back;
    assert(ReadScene(WriteScene(sc) + "garbage\n", back));
    assert(back.actors.size() == 2 && back.actors[0].name == "Button_Map_LichCastle" && back.actors[0].button);
    assert(Near(back.actors[1].scale, 0.004f) && Near(back.cz, 3) && Near(back.actors[1].roll, 3));
    assert(!ReadScene("wm 1 2 3 4 5 6 7 0\n", back));
    std::puts("ok");
}

// just test
#include "../layout_text.hpp"
#include <cassert>
#include <cmath>
#include <cstdio>

static bool Near(float a, float b) { return std::fabs(a - b) < 0.01f; }

int main() {
    using namespace hub_layout;
    auto spots = ParseLayout("# c\nInn_Leena=1.5,2,3,90\r\nbad line\nGuard=4,5,6,-45\nInn_Leena=7,8,9,0\n=1,2,3,4\nX=1,2\n");
    assert(spots.size() == 2 && spots[0].name == "Inn_Leena" && Near(spots[0].x, 7) && spots[1].name == "Guard");
    SetSpot(spots, {"Guard", 0, 0, 0, 10});
    SetSpot(spots, {"Blacksmith_Kadrick", 1, 1, 1, 1});
    assert(spots.size() == 3 && Near(spots[1].yaw, 10));
    auto again = ParseLayout(WriteLayout(spots));
    assert(again.size() == 3 && again[2].name == "Blacksmith_Kadrick" && Near(again[0].z, 9));
    std::puts("ok");
}

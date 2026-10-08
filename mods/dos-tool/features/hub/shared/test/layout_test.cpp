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
    // spawn records round-trip; ids continue after the highest
    std::vector<SpawnRec> rs{{3, "/Game/Env/Meshes/hp_frn_bedA.hp_frn_bedA", 1, 2, 3, 90, 1}, {7, "/Game/x.x", -1, 0, 5, 0, 0.5f}};
    auto rb = ParseSpawns(WriteSpawns(rs) + "junk" + std::string(1, char(10)));
    assert(rb.size() == 2 && rb[0].id == 3 && rb[0].mesh == "/Game/Env/Meshes/hp_frn_bedA.hp_frn_bedA" && Near(rb[1].scale, 0.5f));
    assert(NextSpawnId(rb) == 8 && NextSpawnId({}) == 1);
    std::puts("ok");
}

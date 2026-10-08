// just test
#include "../loot.hpp"
#include <cassert>
#include <cstdio>

int main() {
    using namespace loot;
    // an item on the floor shows until collected; a respawning or hidden one does not
    assert(Unlooted({Kind::Item, false, true, 0}));
    assert(!Unlooted({Kind::Item, false, false, 0}));
    assert(!Unlooted({Kind::Item, true, true, 0}));
    // a chest only while closed and its factory has loot ready
    assert(Unlooted({Kind::Chest, false, true, 0}));
    assert(!Unlooted({Kind::Chest, false, true, 2}));
    assert(!Unlooted({Kind::Chest, false, false, 0}));
    // linear -> sRGB: ends fixed, mid-grey lifts (0.214 linear = 128 sRGB)
    assert(ToSrgb(0) == 0 && ToSrgb(1) == 255 && ToSrgb(2) == 255);
    assert(ToSrgb(0.214f) >= 127 && ToSrgb(0.214f) <= 129);
    std::puts("loot ok");
}

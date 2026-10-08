#include "../path.hpp"
#include <cassert>
#include <cstdio>

using namespace dungeon_map;

static bool Near(float a, float b) { return std::abs(a - b) < 0.01f; }

int main() {
    // L-shaped path: (0,0) → (1000,0) → (1000,1000), length 2000.
    const Path p{{0, 0, 0}, {1000, 0, 0}, {1000, 1000, 0}};
    assert(Near(Length(p), 2000));
    Proj q = Project(p, {500, 100, 0});
    assert(Near(q.s, 500) && Near(q.d, 100));
    q = Project(p, {1100, 700, 0});
    assert(Near(q.s, 1700) && Near(q.d, 100));
    assert(Project(p, {500, 0, 2000}).d > kOffLine);  // a floor above/below does not count
    assert(Near(At(p, 1500).y, 500) && Near(At(p, 9999).y, 1000) && Near(At(p, -5).x, 0));

    // Suffix: the part ahead of s, starting exactly at s, no doubled corner.
    const Path suf = Suffix(p, 500);
    assert(suf.size() == 3 && Near(suf[0].x, 500) && Near(Length(suf), 1500));
    assert(Suffix(p, 1000).size() == 2 && Near(Length(Suffix(p, 1500)), 500) && Suffix(p, -1).empty());

    // Map transform (in game: UnitToPixel 64).
    const Px m = ToMap({51351, 87540, 0}, 64);
    assert(std::abs(m.x - 1367.8f) < 0.1f && std::abs(m.y + 802.4f) < 0.1f);

    // Pulses: spread over the line, faded at both ends, capped.
    auto ps = Pulses(360, 0);
    assert(ps.size() == 10 && Near(ps[0].s, 0) && ps[0].alpha == 0 && ps[5].alpha == 1);
    assert(Pulses(10, 0).empty() && Pulses(100000, 3).size() == size_t(kMaxPulses));
    for (const Pulse& x : Pulses(360, 1.234)) assert(x.s >= 0 && x.s <= 360);

    const Box b{{0, 0, 0}, {100, 10, 200}, 90};
    assert(Inside(b, {0, 90, 0}) && !Inside(b, {90, 0, 0}) && !Inside(b, {0, 90, 500}));
    std::puts("ok");
}

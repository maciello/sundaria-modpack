#include "../plan.hpp"
#include <cassert>
#include <cstdio>

using namespace dungeon_map;

static bool Near(float a, float b) { return std::abs(a - b) < 0.01f; }

int main() {
    // Doors: the closed one nearest the partial end; open or far ones never stop the path.
    std::vector<Trigger> ts{
        {{1000, 0, 0}, 0, true, false, false, false},   // 0 open door at the end
        {{1200, 0, 0}, 0, true, false, true, true},     // 1 closed, locked
        {{5000, 0, 0}, 1, true, false, true, false},    // 2 closed, too far
        {{1100, 300, 0}, 0, false, true, true, false},  // 3 unpulled lever, same room as 1
        {{1100, -300, 0}, 0, false, true, false, false},// 4 pulled lever
        {{4800, 0, 0}, 1, false, true, true, false},    // 5 unpulled lever, other room
    };
    assert(StoppingDoor(ts, {900, 0, 0}) == 1 && StoppingDoor(ts, {9000, 0, 0}) == -1);
    Marks m = MarksFor(ts, 1);
    assert(m.locked && Near(m.door.x, 1200) && m.levers.size() == 1 && Near(m.levers[0].y, 300));
    assert(!MarksFor(ts, 2).locked && !MarksFor(ts, -1).locked);

    const std::vector<Box> rooms{{{0, 0, 0}, {500, 500, 500}, 0}, {{2000, 0, 0}, {500, 500, 500}, 0}};
    assert(RoomOf(rooms, {2100, 0, 0}) == 1 && RoomOf(rooms, {1000, 0, 0}) == -1);

    // Scene: the whole main route, pulses, a cap at the end; icons only for a locked door.
    const Path p{{0, 0, 0}, {6400, 0, 0}, {6400, 6400, 0}};  // 100 px + 100 px at UnitToPixel 64
    auto q = Scene({}, {}, {}, 64, 0, 0);
    assert(q.empty());
    q = Scene(p, {}, {}, 64, 30, 0);
    int bars = 0, caps = 0;
    for (const Quad& x : q) {
        if (x.z == kZLine) bars++;
        if (x.z == kZMark && Near(x.alpha, kCapAlpha)) caps++;
    }
    assert(bars == 2 && caps >= 1);
    const Quad& first = q[0];  // world +X = map up (-y): the first bar is vertical, from (0, 0) to (0, -100)
    assert(Near(first.x, 0) && Near(first.y, -50) && Near(first.w, 100) && Near(std::abs(first.angle), 90));
    const Quad& cap = q.back();
    assert(Near(cap.x, 100) && Near(cap.y, -100) && Near(cap.angle, 15));  // at the end, counter-rotated: 45 - 30
    // Connector: 110 px → 10 dashes of 6 px (period 11), then the join diamond; under the main route's quads.
    const Path link{{0, 6400, 0}, {7040, 6400, 0}};
    q = Scene({}, link, {}, 64, 0, 0);
    int dashes = 0;
    for (const Quad& x : q) dashes += x.z == kZLine && Near(x.w, kDashPx) && Near(x.alpha, kLinkAlpha);
    assert(dashes == 10 && q.size() == 11 && q.back().z == kZMark);
    q = Scene(p, {}, m, 64, 0, 0);
    int icons = 0;
    for (const Quad& x : q) icons += x.z >= kZIcon;
    assert(icons == 3 + 2);  // plate + X, lever plate + handle
    // #83 diagnostic: doors around a partial path's end.
    const std::vector<Trigger> gts{{{3000, 0, 0}, 0, true, false, true, false}, {{500, 0, 0}, 0, true, false, false, false}, {{100, 0, 0}, 0, false, true, true, false}};
    const DoorGap g = DoorsAround(gts, {0, 0, 0});
    assert(g.doors == 2 && g.closed == 1 && Near(g.nearestClosed, 3000) && Near(g.nearestDoor, 500));
    assert(DoorsAround({}, {0, 0, 0}).nearestDoor < 0);
    std::puts("ok");
}

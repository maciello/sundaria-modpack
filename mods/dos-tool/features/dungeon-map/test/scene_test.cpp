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

    // Scene: segments up to the frontier, pulses, a cap; icons only for a locked door.
    const Path p{{0, 0, 0}, {6400, 0, 0}, {6400, 6400, 0}};  // 100 px + 100 px at UnitToPixel 64
    auto q = Scene(p, -1, {}, 64, 0, 0);
    assert(q.empty());
    q = Scene(p, 9600, {}, 64, 30, 0);  // frontier 50 px into the second leg
    int bars = 0, caps = 0;
    for (const Quad& x : q) {
        if (x.z == kZLine) bars++;
        if (x.z == kZMark && Near(x.alpha, kCapAlpha)) caps++;
    }
    assert(bars == 2 && caps >= 1);
    const Quad& first = q[0];  // world +X = map up (-y): the first bar is vertical, centred at (0, -50)
    assert(Near(first.x, 0) && Near(first.y, -50) && Near(first.w, 100) && Near(std::abs(first.angle), 90));
    const Quad& cap = q.back();
    assert(Near(cap.x, 50) && Near(cap.y, -100) && Near(cap.angle, 15));  // counter-rotated: 45 - 30
    q = Scene(p, 9600, m, 64, 0, 0);
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

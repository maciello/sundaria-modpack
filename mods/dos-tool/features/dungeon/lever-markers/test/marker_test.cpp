#include "../marker.hpp"
#include <cassert>
#include <cstdio>

using namespace dungeon_map;

static bool Near(float a, float b, float e = 0.01f) { return std::abs(a - b) < e; }

int main() {
    // Marker: camera at origin looking +X, 90° FOV, 1000×500 screen, inset 20.
    const combat::View v{0, 0, 0, 0, 0, 0, 90};
    Marker m = Place(v, {1000, 0, 0}, 1000, 500, 20);
    assert(!m.edge && Near(m.x, 500) && Near(m.y, 250));
    m = Place(v, {1000, 5000, 0}, 1000, 500, 20);  // far right: right edge, pointing right
    assert(m.edge && Near(m.x, 980) && Near(m.y, 250) && Near(m.angle, 0));
    m = Place(v, {-1000, 0, 500}, 1000, 500, 20);  // behind and above: top edge, pointing up
    assert(m.edge && Near(m.y, 20) && Near(m.angle, -1.5708f, 1e-3f));
    m = Place(v, {-1000, -300, 0}, 1000, 500, 20);  // behind, to the left: left edge
    assert(m.edge && Near(m.x, 20));

    std::puts("ok");
}

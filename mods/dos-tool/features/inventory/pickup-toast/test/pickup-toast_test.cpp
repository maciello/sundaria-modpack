#include "pickup-toast.hpp"
#include <cassert>
#include <cstdio>
#include <cmath>

using namespace pickup_toast;

int main() {
    const Key a{10, 5, 2}, b{11, 5, 3};
    assert(Gained({{a, 1}, {b, 2}}, {{b, 2}, {a, 1}}).empty());       // reorder / sort
    assert(Gained({{a, 1}}, {}).empty());                              // sold, destroyed
    assert((Gained({{a, 1}}, {{a, 2}, {b, 1}}) == std::vector<Key>{a, b}));  // second copy + new item
    assert(Gained({}, {{a, 3}}).size() == 3);

    const float end = kHold + kOut.dur;
    Pose p = PoseAt(0, end);
    assert(p.x == kSlide && p.opacity == 0);
    p = PoseAt(1, end);
    assert(p.x == 0 && p.opacity == 1);
    p = PoseAt(end, end);
    assert(p.opacity == 0);
    assert(PoseAt(end - kOut.dur / 2, end).opacity > 0 && PoseAt(end - kOut.dur / 2, end).opacity < 1);
    assert(RowY(0, 1, 0) == 0 && RowY(0, 1, 1) == -kRowH);
    std::puts("ok");
}

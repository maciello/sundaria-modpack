// just test
#include "roll-cancel.hpp"
#include <cassert>
#include <cstdio>

using namespace roll_cancel;

int main() {
    const Options both;
    assert(Cancel(true, true, Input::Jump, both));
    assert(Cancel(true, true, Input::Ability, both));
    assert(!Cancel(true, true, Input::Other, both));
    assert(!Cancel(false, true, Input::Jump, both));      // not rolling: the game's own handling
    assert(!Cancel(true, false, Input::Jump, both));      // a release never cancels

    Options jumpOnly;
    jumpOnly.abilities = false;
    assert(Cancel(true, true, Input::Jump, jumpOnly) && !Cancel(true, true, Input::Ability, jumpOnly));
    Options none{false, false};
    assert(!Cancel(true, true, Input::Jump, none) && !Cancel(true, true, Input::Ability, none));
    std::puts("ok");
}

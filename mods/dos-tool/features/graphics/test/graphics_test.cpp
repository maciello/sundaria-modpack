// just test
#include "graphics.hpp"
#include <cassert>
#include <cstdio>
#include <cstring>
#include <set>
#include <string>

using namespace graphics;

int main() {
    assert(Command("r.MaxAnisotropy", 16) == "r.MaxAnisotropy 16");
    assert(Command("r.MipMapLODBias", -1) == "r.MipMapLODBias -1");
    assert(Command("r.Tonemapper.Sharpen", 0.6f) == "r.Tonemapper.Sharpen 0.6");
    assert(Command("r.ViewDistanceScale", 1.5f) == "r.ViewDistanceScale 1.5");

    assert(Same(4096, 4096.0001f) && !Same(1.5f, 1.0f) && Same(0, 0.001f));

    // Off: restore only what we changed.
    assert(Next(false, true, State::Applied, 1, 2) == Step::Restore);
    assert(Next(false, true, State::Untouched, 1, 2) == Step::None);
    assert(Next(false, true, State::Rejected, 1, 2) == Step::None);
    // On: capture first, then set until it sticks; rejected rows are left alone.
    assert(Next(true, false, State::Untouched, 1, 2) == Step::Capture);
    assert(Next(true, true, State::Untouched, 1, 2) == Step::Set);
    assert(Next(true, true, State::Applied, 2, 2) == Step::None);
    assert(Next(true, true, State::Applied, 1, 2) == Step::Set);  // game menu re-applied its value
    assert(Next(true, true, State::Rejected, 1, 2) == Step::None);

    // Table sanity: unique cvars, default inside the slider range.
    std::set<std::string> seen;
    for (const Row& r : kRows) {
        assert(seen.insert(r.cvar).second);
        assert(r.lo <= r.value && r.value <= r.hi);
    }
    std::puts("ok");
}

// native: just test
#include "events.hpp"
#include <cassert>
#include <cstdio>

namespace {
    int hits[4];
    void A(void*, void*, void*) { hits[0]++; }
    void B(void*, void*, void*) { hits[1]++; }
    void C(void*, void*, void*) { hits[2]++; }
    void D(void*, void*, void*) { hits[3]++; }
    void Call(events::Cb cb) { cb(nullptr, nullptr, nullptr); }
    events::Sub Fn(const std::string& cls, const std::string& fn, events::Cb cb) { return {events::Kind::Fn, cls, fn, cb}; }
    void Resolve(std::vector<events::Sub>& subs) {  // fake FNames: string length + first char
        for (auto& s : subs) s.fnKey = s.fn.size() * 256 + (s.fn.empty() ? 0 : s.fn[0]), s.clsKey = s.cls.size() * 256 + (s.cls.empty() ? 0 : s.cls[0]), s.resolved = true;
    }
}

int main() {
    using namespace events;
    std::vector<Sub> subs;
    assert(Set(subs, Fn("BP_X_C", "Tick", A), true));
    assert(!Set(subs, Fn("BP_X_C", "Tick", A), true));      // twice = one subscription
    assert(Set(subs, Fn("", "Tick", B), true));             // every class's Tick
    assert(Set(subs, {Kind::Class, "W_C", "", C}, true));
    assert(Set(subs, {Kind::GameTick, "", "", D}, true));
    assert(subs.size() == 4);

    Table t = Build(subs);
    assert(t.unresolved && t.count == 1 && t.gameTick.size() == 1);  // names first become FNames on the game thread
    Resolve(subs);
    t = Build(subs);
    assert(!t.unresolved && t.count == 4);

    const uint64_t tick = 4 * 256 + 'T', x = 6 * 256 + 'B', other = 7 * 256 + 'O', w = 3 * 256 + 'W';
    assert(ForFn(t, tick, x, Call) == 2 && hits[0] == 1 && hits[1] == 1);      // declaring class matches: both
    assert(ForFn(t, tick, other, Call) == 1 && hits[0] == 1 && hits[1] == 2);  // other class: only the any-class one
    assert(ForFn(t, 99, x, Call) == 0);                                        // nobody wants this function
    assert(ForClass(t, w, Call) == 1 && hits[2] == 1);
    assert(ForClass(t, x, Call) == 0);

    assert(Set(subs, Fn("", "Tick", B), false) && !Set(subs, Fn("", "Tick", B), false));
    t = Build(subs);
    assert(ForFn(t, tick, other, Call) == 0);

    for (int i = 0; i < 200; i++) Set(subs, Fn("BP_X_C", "F" + std::to_string(i), A), true);  // no cap (#108: 16, then 64)
    Resolve(subs);
    assert(Build(subs).count == 203);
    std::puts("ok");
}

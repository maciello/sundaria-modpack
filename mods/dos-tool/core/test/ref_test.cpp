// ref::Ref / Cached / Fn against a fake GObjects: collected, slot reused, address reused, re-resolve throttle.
#include "../ref.hpp"
#include <cassert>
#include <cstdio>

namespace {
    struct Obj { int32_t idx; uint64_t name; };
    const void* g_slots[8] = {};
    uint64_t g_now = 0;
    bool g_gameThread = true;
    int g_reloads = 0;
    const void* g_fn = nullptr;  // what detail::Function finds
    void Put(Obj& o) { g_slots[o.idx] = &o; }
}

namespace ref::detail {
    const void* At(int32_t i) { return i >= 0 && i < 8 ? g_slots[i] : nullptr; }
    int32_t IndexOf(const void* o) { return static_cast<const Obj*>(o)->idx; }
    uint64_t NameOf(const void* o) { return static_cast<const Obj*>(o)->name; }
    uint64_t NowMs() { return g_now; }
    bool MayResolve() { return g_gameThread; }
    bool Ok(const void* p) { return p != nullptr; }
    const void* Function(SDK::UClass* (*)(), const char*, const char*) { return g_fn; }
    void Reloaded(const void*, int32_t) { g_reloads++; }
}

int main() {
    Obj a{1, 100};
    Put(a);
    ref::Ref r(&a);
    assert(r.Get<Obj>() == &a && r.Is(&a));
    g_slots[1] = nullptr;  // collected: slot freed
    assert(!r.Get<Obj>() && !r.Is(&a));
    Obj other{1, 200};
    Put(other);            // slot reused by another object
    assert(!r.Get<Obj>());
    g_slots[1] = &a; a.name = 300;  // same address + slot, another object (name differs)
    assert(!r.Get<Obj>());
    a.name = 100;
    assert(r.Get<Obj>() == &a);
    assert(!ref::Ref(nullptr).Get<Obj>());

    // map key: a live object at a collected one's address but another slot is another key
    Obj b{2, 100};
    Put(b);
    assert(!(ref::Ref(&b) == r) && ref::Ref(&a) == r);

    // Fn: resolves, re-resolves after unload, retries a miss at most once per second
    ref::Fn fn{nullptr, "C", "F"};
    Obj f1{3, 7};
    Put(f1);
    g_fn = &f1;
    assert(fn.Get() == reinterpret_cast<SDK::UFunction*>(&f1) && fn.Is(&f1) && g_reloads == 0);
    g_slots[3] = nullptr; g_fn = nullptr;  // class unloaded on map travel
    g_now = 1000;
    assert(!fn.Get());      // miss: resolves (nothing) and arms the retry
    Obj f2{4, 7};
    Put(f2); g_fn = &f2;    // class reloaded elsewhere
    g_now = 1500;
    assert(!fn.Get());      // throttled
    g_now = 2000;
    assert(fn.Is(&f2) && !fn.Is(&f1) && g_reloads == 1);

    // Cached: same contract with a resolver
    static Obj s{5, 9};
    Put(s);
    ref::Cached<Obj> c{[] { return &s; }};
    assert(c.Get() == &s);

    // off the game thread: validate only, never resolve
    ref::Fn off{nullptr, "C", "G"};
    g_gameThread = false;
    g_now = 10000;
    assert(!off.Get() && !off.r.ptr);
    g_gameThread = true;
    assert(off.Get() == reinterpret_cast<SDK::UFunction*>(&f2));
    std::puts("ref_test: ok");
}

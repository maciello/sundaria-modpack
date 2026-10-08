#include "trace.hpp"
#include "logger.hpp"
#include "ref.hpp"

#include <Windows.h>
#include <algorithm>
#include <atomic>
#include <cstdio>
#include <string>
#include <unordered_map>
#include <vector>
#include "CoreUObject_classes.hpp"

using namespace SDK;

namespace {
    struct Entry { ref::Ref fn, cls; int count = 0; double first = 0, last = 0; };

    std::atomic<bool> g_armed{false}, g_active{false};
    // Game thread while active.
    std::unordered_map<uintptr_t, Entry> g_seen;  // key: fn × object class
    double g_start = 0, g_stopAt = 0;
    std::string g_what;

    double Now() { return GetTickCount64() / 1000.0; }
    std::string Name(const ref::Ref& r) {
        const UObject* o = r.Get<UObject>();
        return o ? o->GetName() : "?";
    }
}

namespace cast_trace {
    bool Active() { return g_active.load(std::memory_order_relaxed); }
    void Arm() { g_armed = true; logger::log("[cast-trace] armed: next multi-hit cast"); }

    void Begin(const char* what) {
        if (!g_armed.exchange(false)) return;
        g_seen.clear();
        g_what = what;
        g_start = Now();
        g_stopAt = g_start + kMax;
        g_active = true;
        logger::log("[cast-trace] begin " + g_what);
    }

    void Event(void* obj, void* fn) {
        if (!Active() || g_seen.size() >= 4000) return;
        const auto* o = static_cast<const UObject*>(obj);
        const UObject* cls = ref::detail::Ok(o) ? o->Class : nullptr;
        Entry& e = g_seen[reinterpret_cast<uintptr_t>(fn) * 31 + reinterpret_cast<uintptr_t>(cls)];
        const double t = Now() - g_start;
        if (!e.count) { e.fn = ref::Ref(fn); e.cls = ref::Ref(cls); e.first = t; }
        e.count++;
        e.last = t;
    }

    void End() { if (Active()) g_stopAt = std::min(g_stopAt, Now() + kTail); }

    void Note(const char* line) {
        if (!Active()) return;
        char b[32];
        std::snprintf(b, sizeof b, " +%.2f ", Now() - g_start);
        logger::log(std::string("[cast-trace]") + b + line);
    }

    void Tick() {
        if (!Active() || Now() < g_stopAt) return;
        std::vector<const Entry*> v;
        for (const auto& [k, e] : g_seen) v.push_back(&e);
        std::sort(v.begin(), v.end(), [](const Entry* a, const Entry* b) { return a->count > b->count; });
        std::string out;
        char b[64];
        for (const Entry* e : v) {
            std::snprintf(b, sizeof b, "[cast-trace] x%d +%.2f..%.2f ", e->count, e->first, e->last);
            out += b + Name(e->cls) + "::" + Name(e->fn) + "\r\n";
        }
        logger::log(out + "[cast-trace] end " + g_what + ", " + std::to_string(v.size()) + " functions");
        g_seen.clear();
        g_active = false;
    }
}

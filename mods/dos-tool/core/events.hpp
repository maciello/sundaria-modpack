#pragma once
// Game-event subscriptions (game::On, OnWorldTick, OnClass, OnGameTick, OnEvery in game.hpp): which callbacks one
// ProcessEvent call reaches. SDK-free: names are FName keys (uint64 {ComparisonIndex, Number}) that core/events.cpp
// makes from the strings once, on the game thread. Test: core/test/events_test.cpp.
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace events {
    using Cb = void (*)(void* obj, void* fn, void* parms);
    enum class Kind : uint8_t { Fn, Class, GameTick, Every };

    struct Sub {
        Kind kind;
        std::string cls, fn;  // Fn: cls empty = every class; Class: fn empty
        Cb cb;
        uint64_t clsKey = 0, fnKey = 0;
        bool resolved = false;
        bool Same(const Sub& o) const { return kind == o.kind && cb == o.cb && cls == o.cls && fn == o.fn; }
    };

    // Add (on) or remove (off) one subscription; true = the list changed. Adding twice is one subscription.
    inline bool Set(std::vector<Sub>& subs, const Sub& s, bool on) {
        for (size_t i = 0; i < subs.size(); i++)
            if (subs[i].Same(s)) {
                if (!on) subs.erase(subs.begin() + i);
                return !on;
            }
        if (on) subs.push_back(s);
        return on;
    }

    // Immutable once published: the hook reads it without a lock.
    struct Table {
        struct Hit { uint64_t cls; bool anyCls; Cb cb; };
        std::unordered_map<uint64_t, std::vector<Hit>> byFn;     // function FName -> subscribers
        std::unordered_map<uint64_t, std::vector<Cb>> byClass;   // object's class FName -> subscribers
        std::vector<Cb> gameTick, every;
        bool unresolved = false;  // some names are not FNames yet: core resolves them on the next game-thread call
        size_t count = 0;
    };

    inline Table Build(const std::vector<Sub>& subs) {
        Table t;
        for (const Sub& s : subs) {
            if (!s.resolved && (s.kind == Kind::Fn || s.kind == Kind::Class)) { t.unresolved = true; continue; }
            t.count++;
            switch (s.kind) {
                case Kind::Fn: t.byFn[s.fnKey].push_back({s.clsKey, s.cls.empty(), s.cb}); break;
                case Kind::Class: t.byClass[s.clsKey].push_back(s.cb); break;
                case Kind::GameTick: t.gameTick.push_back(s.cb); break;
                case Kind::Every: t.every.push_back(s.cb); break;
            }
        }
        return t;
    }

    // One ProcessEvent call: fn = the function's FName, outer = its declaring class's FName. O(1) + O(matches).
    template<class F> int ForFn(const Table& t, uint64_t fn, uint64_t outer, F&& call) {
        const auto it = t.byFn.find(fn);
        if (it == t.byFn.end()) return 0;
        int n = 0;
        for (const Table::Hit& h : it->second)
            if (h.anyCls || h.cls == outer) call(h.cb), n++;
        return n;
    }
    template<class F> int ForClass(const Table& t, uint64_t objCls, F&& call) {
        const auto it = t.byClass.find(objCls);
        if (it == t.byClass.end()) return 0;
        for (Cb cb : it->second) call(cb);
        return int(it->second.size());
    }

    // core/events.cpp: called by the ProcessEvent hook in core/game.cpp after the game's own call. Returns callbacks run.
    int Dispatch(const void* obj, void* fn, void* parms);
    bool Live();  // any subscription: the hook must stay installed
    // core/game.cpp
    bool DrainHook();   // wait (bounded) until no ProcessEvent call is inside our hook; false = timed out or called from inside it
    void UpdateHook();  // install/remove the ProcessEvent hook
}

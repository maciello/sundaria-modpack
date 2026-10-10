#pragma once
// Off() hand-off to the game thread for what we put into the world (widgets, actors, particle components): Off() only
// files the removal (Request) and returns; the game thread runs it on the next world tick, from core's pending list or
// the feature's own listener (Serve). Off() runs on the RENDER thread for a menu toggle, and the game thread waits for
// the render thread every frame: blocking there for a world tick can never succeed, and running the removal there is a
// crash (UFunction calls off the game thread, #132). So Request never blocks and never runs f itself.
// Unload (overlay::Shutdown, Present already unhooked, ProcessEvent hook alive) calls Flush() once after all Off()s.
// No tick in time (game unfocused / paused / loading) = the request is dropped and the objects are left to the engine.
// f must own its state: the feature's Off() may not clear what f reads (f clears it on the game thread).
#include <Windows.h>
#include <atomic>
#include <functional>
#include <string>
#include <vector>
#include "game.hpp"
#include "logger.hpp"

namespace game {
    class Drain {
        std::atomic<int> s_{0};  // 0 idle, 1 requested, 2 being served
        std::function<void()> fn_;
        std::string who_;

        struct Pending {
            SRWLOCK mu = SRWLOCK_INIT;
            std::vector<Drain*> list;
            bool listening = false;
            std::atomic<bool> serving{false};
        };
        static Pending& P() {
            static Pending p;
            return p;
        }
        static void Tick(void*, void*, void*) {  // world tick, game thread
            thread_local bool busy = false;
            if (busy || !game::OnGameThread()) return;
            busy = true;
            P().serving = true;
            std::vector<Drain*> todo;
            AcquireSRWLockExclusive(&P().mu);
            todo.swap(P().list);
            ReleaseSRWLockExclusive(&P().mu);
            for (Drain* d : todo) {
                int want = 1;
                if (d->s_.compare_exchange_strong(want, 2)) {
                    d->fn_();
                    d->s_ = 0;
                }
            }
            AcquireSRWLockExclusive(&P().mu);
            const bool idle = P().list.empty();
            if (idle) P().listening = false;
            ReleaseSRWLockExclusive(&P().mu);
            P().serving = false;
            if (idle) game::OnWorldTick(&Tick, false);
            busy = false;
        }
        static void Add(Drain* d) {
            AcquireSRWLockExclusive(&P().mu);
            P().list.push_back(d);
            const bool sub = !P().listening;
            P().listening = true;
            ReleaseSRWLockExclusive(&P().mu);
            if (sub) game::OnWorldTick(&Tick, true);
        }
    public:
        // Game thread, world tick, listener still registered. true = served: end that event, add nothing this tick.
        template <class F> bool Serve(F&& f) {
            int want = 1;
            if (!s_.compare_exchange_strong(want, 2)) return false;
            f();
            s_ = 0;
            return true;
        }
        // Off(), any thread. needed = false: nothing placed. Returns at once; f runs later on the game thread.
        template <class F> void Request(bool needed, const char* who, F f) {
            if (!needed || s_.load() != 0) return;  // already requested: the pending f does the same removal
            fn_ = std::function<void()>(std::move(f));
            who_ = who;
            s_ = 1;
            Add(this);
        }
        bool Idle() const { return s_.load() == 0; }
        bool Cancel() {  // true = it had not run (and now never will)
            int want = 1;
            return s_.compare_exchange_strong(want, 0);
        }
        // Unload only (render thread idle, game thread ticking): wait for every pending request, else drop them.
        static void Flush() {
            for (int i = 0; i < 1000; i++) {
                AcquireSRWLockShared(&P().mu);
                const bool none = P().list.empty();
                ReleaseSRWLockShared(&P().mu);
                if (none && !P().serving.load()) return;
                Sleep(2);
            }
            AcquireSRWLockExclusive(&P().mu);
            std::vector<Drain*> left;
            left.swap(P().list);
            ReleaseSRWLockExclusive(&P().mu);
            for (Drain* d : left) {
                int want = 1;
                if (d->s_.compare_exchange_strong(want, 0)) logger::log("[" + d->who_ + "] no world tick within 2 s: left to the engine");
            }
            if (!left.empty()) game::OnWorldTick(&Tick, false);
            for (int i = 0; i < 250; i++) {  // a tick may be mid-removal
                bool busy = false;
                for (Drain* d : left) busy |= d->s_.load() == 2;
                if (!busy) break;
                Sleep(2);
            }
        }
    };
}

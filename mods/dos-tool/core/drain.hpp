#pragma once
// Off() hand-off to the game thread for what we put into the world (widgets, actors): the game thread removes it on
// a world tick (Serve, from the feature's listener), Off() waits for it (Request). Needs the ProcessEvent hook alive,
// which overlay::Shutdown guarantees for Off() (#84). Without a tick in time the request is dropped (never done off the game thread).
#include <Windows.h>
#include <atomic>
#include <string>
#include "logger.hpp"

namespace game {
    class Drain {
        std::atomic<int> s_{0};  // 0 idle, 1 requested, 2 being served
    public:
        // Game thread, world tick, listener still registered. true = served: end that event, add nothing this tick.
        template <class F> bool Serve(F&& f) {
            int want = 1;
            if (!s_.compare_exchange_strong(want, 2)) return false;
            f();
            s_ = 0;
            return true;
        }
        // Off(), before the listener is unregistered. needed = false skips the wait (nothing placed). true = served.
        // No tick in time (game unfocused / paused / loading): the request is dropped and the objects stay for the engine to
        // collect. Never run f() here: this is not the game thread, and a UFunction call or write to game memory off it
        // crashes the game (#132, hot reload with the game unfocused).
        template <class F> bool Request(bool needed, const char* who, F&&) {
            s_ = needed ? 1 : 0;
            for (int i = 0; i < 1000 && s_.load() == 1; i++) Sleep(2);
            int want = 1;
            if (s_.compare_exchange_strong(want, 0)) {
                logger::log(std::string("[") + who + "] no world tick within 2 s: left to the engine");
                return false;
            }
            for (int i = 0; i < 250 && s_.load() == 2; i++) Sleep(2);  // a tick is mid-removal: let it finish
            return true;
        }
    };
}

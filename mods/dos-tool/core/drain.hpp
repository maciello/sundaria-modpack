#pragma once
// Off() hand-off to the game thread for what we put into the world (widgets, actors): the game thread removes it on
// a world tick (Serve, from the feature's listener), Off() waits for it (Request). Needs the ProcessEvent hook alive,
// which overlay::Shutdown guarantees for Off() (#84). Without a tick in time the caller does the removal itself.
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
        // Off(), before the listener is unregistered. needed = false skips the wait (nothing placed).
        template <class F> void Request(bool needed, const char* who, F&& f) {
            s_ = needed ? 1 : 0;
            for (int i = 0; i < 250 && s_.load() != 0; i++) Sleep(2);
            int want = 1;
            if (s_.compare_exchange_strong(want, 2)) {
                f();
                s_ = 0;
                logger::log(std::string("[") + who + "] removed off the game thread (no world tick within 500 ms)");
            }
            for (int i = 0; i < 250 && s_.load() == 2; i++) Sleep(2);  // a tick is mid-removal: let it finish
        }
    };
}

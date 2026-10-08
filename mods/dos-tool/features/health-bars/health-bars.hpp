#pragma once
#include <algorithm>
#include <unordered_map>
#include <vector>
#include "combat.hpp"

// SDK-free health-bar state: fill fraction + a trailing "chip" that holds briefly after a hit,
// then drains to the fill. Max HP = max(attribute, peak HP seen) since the attribute is unverified.
namespace health_bars {
    struct Bar { uintptr_t id; float x, y, z; float frac, chip; };

    struct Bars {
        double chipDelay = 0.4;   // s the chip holds after a hit
        float chipRate = 0.8f;    // fraction of the bar drained per second
        double linger = 4.0;      // s a full-HP bar stays after its last change (e.g. healed up)

        struct State { float max = 0, shown = -1, chip = -1; double changed = -1e9, hit = -1e9; };
        std::unordered_map<uintptr_t, State> st;

        std::vector<Bar> Update(const std::vector<combat::Sample>& samples, double now, double dt) {
            std::vector<Bar> out;
            std::unordered_map<uintptr_t, State> next;
            for (const combat::Sample& s : samples) {
                if (s.isPlayer) continue;
                State b = st.count(s.id) ? st[s.id] : State{};
                b.max = std::max({b.max, s.maxHealth, s.health});
                const float frac = b.max > 0 ? std::clamp(s.health / b.max, 0.0f, 1.0f) : 0.0f;
                if (b.shown < 0) b.shown = b.chip = frac;
                if (frac != b.shown) b.changed = now;
                if (frac < b.shown) b.hit = now;
                b.shown = frac;
                b.chip = std::max(b.chip, frac);
                if (now - b.hit > chipDelay) b.chip = std::max(frac, b.chip - chipRate * float(dt));
                next[s.id] = b;
                if (s.health > 0 && (frac < 0.999f || now - b.changed < linger))
                    out.push_back({s.id, s.x, s.y, s.z, frac, b.chip});
            }
            st.swap(next);
            return out;
        }
    };
}

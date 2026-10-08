#pragma once
#include <algorithm>
#include <unordered_map>
#include <vector>
#include "combat.hpp"

// SDK-free health-bar state. A bar appears on an enemy's first hit, fades in, shows a fill plus a
// white "chip" that holds briefly then drains, flashes on each hit, and fades out once the enemy is
// back at full HP (after `linger`) or dead. Max HP = peak HP seen: the Secondary Health attribute exceeds
// CurrentHealth on unhit enemies, so it is not max HP. Hidden while the enemy is not on screen (behind a wall).
namespace health_bars {
    struct Bar { uintptr_t id; float x, y, z; float frac, chip, alpha, flash, level; };

    struct Bars {
        double chipDelay = 0.4;   // s the chip holds after a hit
        float chipRate = 0.8f;    // bar fraction drained per second
        double fadeIn = 0.15, fadeOut = 0.4;
        double linger = 3.0;      // s a full-HP bar stays after its last change
        double flashTime = 0.18;
        float occludedAfter = 0.2f; // s an enemy may miss the screen before its bar hides

        struct State { float max = 0, shown = -1, chip = -1; double changed = -1e9, hit = -1e9, appeared = -1, gone = -1; };
        std::unordered_map<uintptr_t, State> st;

        static float Clamp01(double x) { return float(std::clamp(x, 0.0, 1.0)); }

        std::vector<Bar> Update(const std::vector<combat::Sample>& samples, double now, double dt) {
            std::vector<Bar> out;
            std::unordered_map<uintptr_t, State> next;
            float newest = 0;  // latest on-screen time of any character = "now" on the game's render clock
            for (const combat::Sample& s : samples) newest = std::max(newest, s.seen);
            for (const combat::Sample& s : samples) {
                if (s.isPlayer) continue;
                State b = st.count(s.id) ? st[s.id] : State{};
                b.max = std::max(b.max, s.health);
                const float frac = b.max > 0 ? std::clamp(s.health / b.max, 0.0f, 1.0f) : 0.0f;
                if (b.shown < 0) b.shown = b.chip = frac;
                if (frac != b.shown) b.changed = now;
                if (frac < b.shown) b.hit = now;
                b.shown = frac;
                b.chip = std::max(b.chip, frac);
                if (now - b.hit > chipDelay) b.chip = std::max(frac, b.chip - chipRate * float(dt));

                const bool wanted = s.health > 0 && b.hit > 0 && (frac < 0.999f || now - b.changed < linger);
                if (wanted && b.appeared < 0) { b.appeared = now; b.gone = -1; }
                if (!wanted && b.appeared >= 0 && b.gone < 0) b.gone = now;
                if (b.gone >= 0 && now - b.gone >= fadeOut) b.appeared = b.gone = -1;
                next[s.id] = b;
                if (b.appeared < 0 || (newest > 0 && s.seen < newest - occludedAfter)) continue;
                float alpha = Clamp01((now - b.appeared) / fadeIn);
                if (b.gone >= 0) alpha *= 1.0f - Clamp01((now - b.gone) / fadeOut);
                const float flash = 1.0f - Clamp01((now - b.hit) / flashTime);
                out.push_back({s.id, s.x, s.y, s.z, frac, b.chip, alpha, flash, s.level});
            }
            st.swap(next);
            return out;
        }
    };
}

#pragma once
// Which abilities a hero casts in a fight of T seconds, given damage per cast, cast time and cooldown per ability.
// Priority = damage per cast-second. Fluid (default): each ability takes min(its cooldown-limited time share, what
// is left), smooth in every stat. Event: discrete greedy simulation (phase-sensitive).
#include "layout.hpp"
#include <algorithm>

namespace dps::rotation {
    // fills uses[k] (casts in T), returns total damage
    inline float Run(int K, const float* dmg, const float* cast, const float* cd, float T, bool event, float* uses) {
        int ord[layout::kMaxAbilities];
        for (int k = 0; k < K; ++k) { ord[k] = k; uses[k] = 0.f; }
        std::sort(ord, ord + K, [&](int i, int j) { return dmg[i] * cast[j] > dmg[j] * cast[i]; });
        float total = 0.f;
        if (!event) {
            float rem = 1.f;
            for (int o = 0; o < K && rem > 0.f; ++o) {
                const int k = ord[o];
                if (dmg[k] <= 0.f) continue;
                // casts = 1 + (T - cast) / cd (first cast at t = 0)
                const float share = cd[k] <= cast[k] ? 1.f : std::min(1.f, (1.f + std::max(0.f, T - cast[k]) / cd[k]) * cast[k] / T);
                const float use = std::min(share, rem);
                rem -= use;
                uses[k] = use * T / cast[k];
                total += uses[k] * dmg[k];
            }
            return total;
        }
        int KK = K;   // abilities below one that is always ready (cd <= cast) never fire
        for (int o = 0; o < K; ++o)
            if (cd[ord[o]] <= cast[ord[o]] && dmg[ord[o]] > 0.f) { KK = o + 1; break; }
        float ready[layout::kMaxAbilities] = {0};
        float t = 0.f;
        while (t < T) {
            int pick = -1;
            float next = 1e30f;
            for (int o = 0; o < KK; ++o) {
                const int k = ord[o];
                if (dmg[k] <= 0.f) continue;
                if (ready[k] <= t + 1e-4f) { pick = k; break; }
                next = std::min(next, ready[k]);
            }
            if (pick < 0) { if (next >= 1e30f) break; t = next; continue; }
            const float frac = t + cast[pick] <= T ? 1.f : (T - t) / cast[pick];
            total += dmg[pick] * frac;
            uses[pick] += frac;
            ready[pick] = t + cd[pick];
            t += cast[pick];
        }
        return total;
    }
}

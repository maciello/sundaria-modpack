#pragma once
// SDK-free logic of the idle loot sparkle (#27): WoW lootable-corpse style. Now and then, at random intervals, a pile
// of unlooted loot lets off a small burst of tiny motes that rise and twinkle out, in the colour of its best item grade.
// Spec: references/design-system.md § Loot marker (idle sparkle). Geometry in units (px at 1080p, × Ui × depth).
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>
#include "style.hpp"
#include "../shared/loot.hpp"

namespace idle_loot {
    constexpr float kCull = 3000, kFadeFrom = 2500;   // cm: drawn up to 30 m, fading from 25 m
    constexpr float kPileR = 150;                      // cm: loot this close is one pile, one sparkle, its best grade
    constexpr float kLiftItem = 15, kLiftChest = 45;   // cm above the root where motes are born
    constexpr float kOccludedAfter = 0.15f;            // s the mesh may miss the screen before it counts as hidden

    // Burst schedule: a pile's bursts are kGapMin..kGapMax apart (uniform), the first one 0..kGapMax after it appears.
    constexpr float kGapMin = 2.5f, kGapMax = 7.0f;
    constexpr int kMotesMin = 7, kMotesMax = 13;       // per burst
    constexpr float kStagger = 0.4f;                   // s: a burst's motes are born over this window
    constexpr float kLifeMin = 0.7f, kLifeMax = 1.3f;  // s per mote
    constexpr float kSpreadX = 18, kSpreadY = 8;       // units: birth area (half-widths) around the anchor
    constexpr float kRiseMin = 10, kRiseMax = 26;      // units/s upwards
    constexpr float kDrift = 5;                        // units/s sideways, either way
    constexpr float kMoteMinR = 1.0f, kMoteMaxR = 2.2f; // units: dot radius
    constexpr float kGlintEvery = 4;                   // ~1 mote in 4 is a cross glint
    constexpr float kGlintR = 4.5f;                    // units: glint star radius
    constexpr float kHaloScale = 2.2f, kHaloA = 0.28f; // tier-coloured halo disc behind each mote
    constexpr float kCoreWhite = 0.55f;                // mote core = tier colour mixed this far to white
    constexpr float kBurstLen = kStagger + kLifeMax;   // s from a burst's start until its last mote is gone

    inline std::uint32_t Hash(std::uint64_t x) {  // splitmix64, folded
        x += 0x9E3779B97F4A7C15ull;
        x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ull;
        x = (x ^ (x >> 27)) * 0x94D049BB133111EBull;
        return std::uint32_t((x ^ (x >> 31)) >> 7);
    }
    inline float Unit(std::uint32_t h) { return float(h & 0xFFFFFF) / float(0x1000000); }  // [0, 1)
    inline float Lerp(float a, float b, float t) { return a + (b - a) * t; }
    inline std::uint64_t Seed(std::uint64_t pile, std::uint32_t burst, std::uint32_t salt) {
        return pile * 0x100000001B3ull ^ (std::uint64_t(burst) << 20) ^ salt;
    }

    // Seconds until burst n + 1 of a pile (n = 0: until its first).
    inline float Gap(std::uint64_t pile, std::uint32_t n) {
        const float u = Unit(Hash(Seed(pile, n, 1)));
        return n == 0 ? u * kGapMax : Lerp(kGapMin, kGapMax, u);
    }
    inline int Motes(std::uint64_t pile, std::uint32_t burst) {
        return kMotesMin + int(Unit(Hash(Seed(pile, burst, 2))) * float(kMotesMax - kMotesMin + 1));
    }

    struct Mote { float dx, dy, r, a; bool glint; };  // offset in units (y down), radius in units, alpha 0..1
    // Mote k of a burst, age s after the burst started. a == 0: not born yet or gone.
    inline Mote MoteAt(std::uint64_t pile, std::uint32_t burst, int k, float age) {
        std::uint32_t h = Hash(Seed(pile, burst, 16 + std::uint32_t(k)));
        auto next = [&h] { h = Hash(h); return Unit(h); };
        const float born = next() * kStagger, life = Lerp(kLifeMin, kLifeMax, next());
        const float t = age - born;
        if (t < 0 || t >= life) return {0, 0, 0, 0, false};
        const float s = t / life;
        const float x0 = (next() * 2 - 1) * kSpreadX, y0 = (next() * 2 - 1) * kSpreadY;
        const float rise = Lerp(kRiseMin, kRiseMax, next()), drift = (next() * 2 - 1) * kDrift;
        const bool glint = next() * kGlintEvery < 1;
        const float r = glint ? kGlintR : Lerp(kMoteMinR, kMoteMaxR, next());
        const float a = std::sin(3.14159265f * s);  // twinkle in and out
        return {x0 + drift * t, y0 - rise * t, r * (glint ? a : 1 - 0.5f * s), a, glint};
    }

    // Unlooted loot grouped into piles (greedy, by the first member within kPileR). O(n²) over the near loot.
    struct Pile {
        std::uint64_t key;   // smallest member id: stable while that member lies there
        float x, y, z;       // first member's position
        int grade;           // best known EItemGrade (by rank: Poor 0 .. Eternal 7), -1 none known
        bool chest;
        bool onScreen;       // any member rendered recently
        int members;
    };
    // seen NaN = no mesh to ask; newest <= 0 = no reference time this frame: both count as visible.
    inline bool OnScreen(float seen, float newest) { return std::isnan(seen) || newest <= 0 || seen >= newest - kOccludedAfter; }

    inline void Piles(const std::vector<loot::Actor>& seen, float newest, std::vector<Pile>& out) {
        // ponytail: O(n²) greedy grouping; n = loot within 30 m (a dungeon streams 1–9 at a time)
        out.clear();
        std::vector<const loot::Actor*> sorted;
        for (const loot::Actor& a : seen) if (a.unlooted) sorted.push_back(&a);
        std::sort(sorted.begin(), sorted.end(), [](auto* p, auto* q) { return p->id < q->id; });
        for (const loot::Actor* a : sorted) {
            Pile* in = nullptr;
            for (Pile& p : out) {
                const float dx = a->x - p.x, dy = a->y - p.y, dz = a->z - p.z;
                if (dx * dx + dy * dy + dz * dz <= kPileR * kPileR) { in = &p; break; }
            }
            const bool vis = OnScreen(a->seen, newest), chest = a->kind == loot::Kind::Chest;
            if (!in) { out.push_back({a->id, a->x, a->y, a->z, a->grade, chest, vis, 1}); continue; }
            in->grade = std::max(in->grade, a->grade);
            in->chest |= chest;
            in->onScreen |= vis;
            in->members++;
        }
    }

    inline float Depth(float dist) { return std::clamp(1500.0f / std::max(dist, 1.0f), 0.6f, 1.25f); }
    inline float DistFade(float dist) { return std::clamp((kCull - dist) / (kCull - kFadeFrom), 0.0f, 1.0f); }

    // Moves a toward target: in over kFadeIn, out over kFadeOut (linear in time; eased when drawn).
    inline float Approach(float a, float target, float dt) {
        const float dur = target > a ? style::motion::kFadeIn.dur : style::motion::kFadeOut.dur;
        const float step = dt / dur;
        return target > a ? std::min(target, a + step) : std::max(target, a - step);
    }
}

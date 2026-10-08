#pragma once
// SDK-free logic of the idle loot sparkle (#27): breathing glow + twinkling glints over unlooted loot.
// Spec: references/design-system.md § Loot marker (idle shimmer). Geometry in units (px at 1080p, × Ui × depth).
#include <algorithm>
#include <cmath>
#include <cstdint>
#include "style.hpp"

namespace idle_loot {
    constexpr float kCull = 3000, kFadeFrom = 2500;   // cm: drawn up to 30 m, fading from 25 m
    constexpr float kGlowR[3] = {10, 16, 24};          // concentric glow discs
    constexpr float kGlowA[3] = {0.30f, 0.15f, 0.07f};
    constexpr float kGlintR = 7;                       // sparkle glyph radius
    constexpr float kHaloScale = 1.8f;                 // tier-coloured halo disc behind a glint, × glyph radius
    constexpr float kCoreScale = 0.5f;                 // white hot core inside the tier-coloured glint
    constexpr int kGlints = 2;                         // per shimmer period, evenly staggered
    constexpr float kGlintLife = 0.5f;                 // s: scale 0 -> 1 -> 0, InOutCubic
    constexpr float kAreaRx = 18, kAreaUp = 26, kAreaDown = 4;  // glints appear in this box around the anchor
    constexpr float kLiftItem = 15, kLiftChest = 45;   // cm above the root where glints centre
    constexpr float kOccludedAfter = 0.15f;            // s the mesh may miss the screen before it counts as hidden

    inline std::uint32_t Hash(std::uint64_t x) {  // splitmix64, folded
        x += 0x9E3779B97F4A7C15ull;
        x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ull;
        x = (x ^ (x >> 27)) * 0x94D049BB133111EBull;
        return std::uint32_t((x ^ (x >> 31)) >> 7);
    }
    inline float Unit(std::uint32_t h) { return float(h & 0xFFFFFF) / float(0x1000000); }  // [0, 1)

    // Phase offset in [0, 1) per actor, so neighbouring markers never sync.
    inline float Phase(std::uint64_t id) { return Unit(Hash(id)); }

    // Glow alpha multiplier: sine between 0.75 and 1.0 at kPulsePeriod.
    inline float Breathe(double t, float phase) {
        const double x = t / style::motion::kPulsePeriod + phase;
        return 0.875f + 0.125f * float(std::sin(6.283185307179586 * x));
    }

    struct Glint { float dx, dy, scale; };  // offset in units (y down), scale 0..1
    // Glint k of an actor at time t: born once per kShimmerPeriod at a fresh spot, lives kGlintLife.
    inline Glint GlintAt(std::uint64_t id, double t, int k) {
        const double period = style::motion::kShimmerPeriod;
        const double x = t / period + Phase(id) + double(k) / kGlints;
        const double cycle = std::floor(x);
        const float age = float((x - cycle) * period);
        if (age >= kGlintLife) return {0, 0, 0};
        const std::uint32_t h = Hash(id * 31 + std::uint64_t(int64_t(cycle)) * 8 + std::uint64_t(k));
        const float u = Unit(h), v = Unit(Hash(h));
        const float s = age / kGlintLife;
        const float scale = s < 0.5f ? style::ease::Apply(style::ease::Curve::InOutCubic, s * 2)
                                     : style::ease::Apply(style::ease::Curve::InOutCubic, (1 - s) * 2);
        return {(u * 2 - 1) * kAreaRx, -kAreaUp + v * (kAreaUp + kAreaDown), scale};
    }

    inline float Depth(float dist) { return std::clamp(1500.0f / std::max(dist, 1.0f), 0.6f, 1.25f); }
    inline float DistFade(float dist) { return std::clamp((kCull - dist) / (kCull - kFadeFrom), 0.0f, 1.0f); }

    // Moves a toward target: in over kFadeIn, out over kFadeOut (linear in time; eased when drawn).
    inline float Approach(float a, float target, float dt) {
        const float dur = target > a ? style::motion::kFadeIn.dur : style::motion::kFadeOut.dur;
        const float step = dt / dur;
        return target > a ? std::min(target, a + step) : std::max(target, a - step);
    }

    // Visible on screen: its mesh rendered within kOccludedAfter of the newest render time seen this frame.
    // seen NaN = no mesh to ask; newest <= 0 = no reference time this frame: both count as visible.
    inline bool OnScreen(float seen, float newest) { return std::isnan(seen) || newest <= 0 || seen >= newest - kOccludedAfter; }
}

#pragma once
// SDK-free logic of the pickup toast (#64): which items are new, and where each toast is drawn at a given age.
#include <algorithm>
#include <map>
#include <vector>
#include "style.hpp"

namespace pickup_toast {
    // An owned item as far as a pickup can tell: no instance id exists, and the game rewrites ChangedID on reorder.
    struct Key {
        int spec, level, grade;
        auto operator<=>(const Key&) const = default;
    };
    using Owned = std::map<Key, int>;  // count per key: inventory + equipped + bank

    // One entry per copy that `after` has more of than `before`. Reorders, sorts, equips, bank moves: none.
    inline std::vector<Key> Gained(const Owned& before, const Owned& after) {
        std::vector<Key> out;
        for (const auto& [k, n] : after) {
            const auto it = before.find(k);
            for (int i = it == before.end() ? 0 : it->second; i < n; i++) out.push_back(k);
        }
        return out;
    }

    // Layout in Slate units (1080p = 1). Newest toast at row 0 (the anchor); older ones move up a row.
    constexpr float kAnchorX = 0.64f, kAnchorY = 0.56f;  // viewport fraction: right of the character, below centre
    constexpr float kRowH = 48, kSlide = 56;             // row pitch; slide-in distance from the right
    constexpr int kMaxRows = 5;                          // a 6th pushes the oldest out
    constexpr float kHold = 4.0f;                        // seconds on screen before the fade
    constexpr style::motion::Motion kIn{0.28f, style::ease::Curve::OutCubic};
    constexpr style::motion::Motion kRowMove{0.18f, style::ease::Curve::OutCubic};
    constexpr style::motion::Motion kOut = style::motion::kFadeOut;

    struct Pose { float x, opacity; };
    // age: seconds since shown; end: age at which the fade-out completes.
    inline Pose PoseAt(float age, float end) {
        using style::ease::Apply;
        const float in = Apply(kIn.curve, age / kIn.dur);
        const float out = 1 - Apply(kOut.curve, (age - (end - kOut.dur)) / kOut.dur);
        return {kSlide * (1 - in), std::clamp(std::min(in, out), 0.0f, 1.0f)};
    }
    // Row offset easing from `from` to `to` rows, t seconds after the row changed.
    inline float RowY(float from, float to, float t) {
        return -kRowH * (from + (to - from) * style::ease::Apply(kRowMove.curve, t / kRowMove.dur));
    }
}

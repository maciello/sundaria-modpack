#pragma once
#include <algorithm>

// SDK-free logic for Overwatch movement: blend the game's ground values toward snappy ones.
namespace ow_movement {
    // Same fields as game::Ground (kept SDK- and core-free for the test).
    struct Ground { float maxAccel, brakingWalking, groundFriction, brakingFrictionFactor, jumpZ; };

    // Overwatch-like: full speed and a full stop in under ~0.1 s at walk speed (~600 cm/s).
    constexpr float kSnapAccel = 12000.0f;   // cm/s²  (UE default 2048)
    constexpr float kSnapBraking = 12000.0f; // cm/s²  (UE default 2048)
    constexpr float kSnapFriction = 12.0f;   //        (UE default 8)

    inline float Lerp(float a, float b, float t) { return a + (b - a) * t; }

    // snap 0 = vanilla, 1 = Overwatch-like; never makes a value worse than vanilla.
    // jumpScale multiplies vanilla jump height.
    inline Ground Blend(const Ground& vanilla, float snap, float jumpScale) {
        const float t = std::clamp(snap, 0.0f, 1.0f);
        Ground g = vanilla;
        g.maxAccel = Lerp(vanilla.maxAccel, std::max(vanilla.maxAccel, kSnapAccel), t);
        g.brakingWalking = Lerp(vanilla.brakingWalking, std::max(vanilla.brakingWalking, kSnapBraking), t);
        g.groundFriction = Lerp(vanilla.groundFriction, std::max(vanilla.groundFriction, kSnapFriction), t);
        g.jumpZ = vanilla.jumpZ * std::clamp(jumpScale, 0.5f, 2.0f);
        return g;
    }

    // Seconds from standstill to speed v at acceleration a (friction ignored): for the menu.
    inline float TimeTo(float v, float a) { return a > 0 ? v / a : 0; }
}

#pragma once
#include <cmath>
#include <cstdio>
#include <sstream>
#include <string>
#include <vector>

// SDK-free tavern-hub logic (UE axes: X forward, Y right, Z up; degrees).
// Walk: a third-person camera orbits the hero; WASD move relative to the camera's yaw.
// Layout: where each NPC stands, one "name=x,y,z,yaw" line per NPC (dos-tool-tavern.ini next to the game exe).
namespace tavern_hub {
    constexpr float kD2R = 3.14159265f / 180.0f;

    struct Pose { float x, y, z, pitch, yaw; };

    // Camera behind the hero's head at `dist`, looking at it with `pitch` (negative = from above).
    inline Pose Follow(float hx, float hy, float hz, float yaw, float pitch, float dist, float height) {
        const float cp = std::cos(pitch * kD2R), sp = std::sin(pitch * kD2R);
        const float cy = std::cos(yaw * kD2R), sy = std::sin(yaw * kD2R);
        const float tz = hz + height;
        return {hx - dist * cp * cy, hy - dist * cp * sy, tz - dist * sp, pitch, yaw};
    }

    // WASD (forward/right in -1..1) relative to camera yaw → world XY direction, length ≤ 1.
    inline void MoveDir(float forward, float right, float camYaw, float& x, float& y) {
        const float cy = std::cos(camYaw * kD2R), sy = std::sin(camYaw * kD2R);
        x = forward * cy - right * sy;
        y = forward * sy + right * cy;
        const float len = std::sqrt(x * x + y * y);
        if (len > 1.0f) { x /= len; y /= len; }
    }

    // Yaw that makes something at (fx,fy) face (tx,ty).
    inline float FaceYaw(float fx, float fy, float tx, float ty) { return std::atan2(ty - fy, tx - fx) / kD2R; }
}

#pragma once
#include <cmath>
#include <vector>

// SDK-free view math for the hub features (UE axes: X forward, Y right, Z up; degrees): what the camera aims at,
// where the view meets a floor, what the crosshair is on.
namespace hub_aim {
    constexpr float kD2R = 3.14159265f / 180.0f;

    // Index of the target the camera looks at: smallest angle between the view direction and the direction to it,
    // within maxDeg and maxDist; aimZ lifts each target point (feet/centre → chest). -1 = none.
    struct Target { float x, y, z; };
    inline int LookedAt(float cx, float cy, float cz, float pitch, float yaw, const std::vector<Target>& ts,
                        float maxDeg, float maxDist, float aimZ) {
        const float cp = std::cos(pitch * kD2R), fx = cp * std::cos(yaw * kD2R), fy = cp * std::sin(yaw * kD2R), fz = std::sin(pitch * kD2R);
        const float minCos = std::cos(maxDeg * kD2R);
        int best = -1;
        float bestCos = minCos;
        for (int i = 0; i < int(ts.size()); i++) {
            const float dx = ts[i].x - cx, dy = ts[i].y - cy, dz = ts[i].z + aimZ - cz;
            const float d = std::sqrt(dx * dx + dy * dy + dz * dz);
            if (d < 1.0f || d > maxDist) continue;
            const float c = (dx * fx + dy * fy + dz * fz) / d;
            if (c > bestCos) { bestCos = c; best = i; }
        }
        return best;
    }

    // Where the view ray from the camera meets the horizontal floor at floorZ (looking down at it, within maxDist).
    inline bool RayToFloor(float cx, float cy, float cz, float pitch, float yaw, float floorZ, float maxDist, float& x, float& y) {
        const float cp = std::cos(pitch * kD2R), dz = std::sin(pitch * kD2R);
        if (dz > -0.02f || cz <= floorZ) return false;  // not looking down at that floor
        const float t = (floorZ - cz) / dz;
        if (t > maxDist) return false;
        x = cx + t * cp * std::cos(yaw * kD2R);
        y = cy + t * cp * std::sin(yaw * kD2R);
        return true;
    }

    // What the crosshair is on: the nearest sphere (centre + radius) the view ray passes through, within maxDist.
    // -1 = none. (A prop behind another one along the ray loses: nearest hit wins.)
    struct Sphere { float x, y, z, r; };
    inline int RayPick(float cx, float cy, float cz, float pitch, float yaw, const std::vector<Sphere>& ss, float maxDist) {
        const float cp = std::cos(pitch * kD2R), dx = cp * std::cos(yaw * kD2R), dy = cp * std::sin(yaw * kD2R), dz = std::sin(pitch * kD2R);
        int best = -1;
        float bestT = maxDist;
        for (int i = 0; i < int(ss.size()); i++) {
            const float ox = ss[i].x - cx, oy = ss[i].y - cy, oz = ss[i].z - cz;
            const float t = ox * dx + oy * dy + oz * dz;            // along the ray to the closest point
            if (t <= 0) continue;
            const float d2 = ox * ox + oy * oy + oz * oz - t * t;    // squared miss distance
            if (d2 > ss[i].r * ss[i].r) continue;
            const float hit = t - std::sqrt(ss[i].r * ss[i].r - d2); // where the ray enters it
            if (hit < bestT) { bestT = hit; best = i; }
        }
        return best;
    }
}

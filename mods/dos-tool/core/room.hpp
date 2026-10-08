#pragma once
#include <algorithm>
#include <vector>

// SDK-free: invisible collision for a hub building that has none (the _HUB meshes are visual-only).
// In the building's local space (its mesh bounds): a floor slab whose top is at `floorZ`, and four walls of
// height `wallHeight` standing on it, each moved inside the bounds by its own inset (the bounds include roof
// overhang and pillars, so the visible walls sit further in, by a different amount per side).
namespace room {
    struct Box { float cx, cy, cz, ex, ey, ez; };  // centre + half extents, local units

    struct Insets { float minX, maxX, minY, maxY; };

    inline std::vector<Box> Shell(float minX, float minY, float maxX, float maxY, float floorZ,
                                  const Insets& in, float thickness, float wallHeight) {
        const float x0 = minX + in.minX, x1 = maxX - in.maxX, y0 = minY + in.minY, y1 = maxY - in.maxY;
        if (x1 - x0 < 2 * thickness || y1 - y0 < 2 * thickness) return {};
        const float mx = (x0 + x1) / 2, my = (y0 + y1) / 2, hx = (x1 - x0) / 2, hy = (y1 - y0) / 2;
        const float t = thickness / 2, wz = floorZ + wallHeight / 2, hz = wallHeight / 2;
        return {
            {mx, my, floorZ - t, hx, hy, t},  // floor
            {x0 + t, my, wz, t, hy, hz},      // -X wall
            {x1 - t, my, wz, t, hy, hz},      // +X wall
            {mx, y0 + t, wz, hx, t, hz},      // -Y wall
            {mx, y1 - t, wz, hx, t, hz},      // +Y wall
        };
    }

    // Is a local point inside the bounds' footprint (and not far below / above the building)?
    inline bool Inside(float px, float py, float pz, float minX, float minY, float minZ, float maxX, float maxY, float maxZ) {
        return px >= minX && px <= maxX && py >= minY && py <= maxY && pz >= minZ - 300 && pz <= maxZ;
    }
}

#pragma once
#include <cmath>

// SDK-free: the world map diorama shrunk onto a desk. Every scene actor keeps its place relative to the diorama's
// centre, scaled by k and turned by the desk's yaw; its own scale multiplies by k and its yaw adds the desk's.
namespace mini_map {
    struct Xf { float x, y, z, yaw, scale; };

    inline Xf Shrink(const Xf& a, float cx, float cy, float cz, float deskX, float deskY, float deskZ, float deskYaw, float k) {
        const float r = deskYaw * 3.14159265f / 180.0f, c = std::cos(r), s = std::sin(r);
        const float dx = (a.x - cx) * k, dy = (a.y - cy) * k, dz = (a.z - cz) * k;
        return {deskX + dx * c - dy * s, deskY + dx * s + dy * c, deskZ + dz, a.yaw + deskYaw, a.scale * k};
    }
}

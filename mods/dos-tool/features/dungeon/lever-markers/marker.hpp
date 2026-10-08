#pragma once
// SDK-free: where a lever's world marker goes (#93): over the lever, or clamped to the screen edge pointing at it.
// Spec: design-system.md § Dungeon map path (levers).
#include <algorithm>
#include <cmath>
#include "combat.hpp"
#include "../shared/path.hpp"

namespace dungeon_map {
    constexpr float kLeverRange = 6000;  // world marker within 60 m of the player
    constexpr float kLeverNear = 200;    // hidden at 2 m (you are there)
    constexpr float kLeverLift = 120;    // marker this far above the lever origin
    constexpr float kMarkerPx = 12, kChevronLen = 9, kChevronW = 3;  // × Ui

    // Screen spot of the marker: on screen inside the inset, else on the inset edge in the lever's direction (edge = true,
    // angle = radians, screen space, pointing at the lever).
    struct Marker { float x = 0, y = 0, angle = 0; bool edge = false; };
    inline Marker Place(const combat::View& v, V3 p, float w, float h, float inset) {
        float sx, sy, dx, dy;
        const float cx = w * 0.5f, cy = h * 0.5f;
        if (combat::Project(v, p.x, p.y, p.z, w, h, sx, sy)) {
            if (sx >= inset && sx <= w - inset && sy >= inset && sy <= h - inset) return {sx, sy, 0, false};
            dx = sx - cx, dy = sy - cy;
        } else if (combat::Project(v, 2 * v.x - p.x, 2 * v.y - p.y, 2 * v.z - p.z, w, h, sx, sy)) {
            dx = cx - sx, dy = cy - sy;  // behind the camera: the point mirrored through it lands opposite
        } else {
            dx = 0, dy = 1;  // level with the camera plane: point down
        }
        if (std::abs(dx) < 1e-3f && std::abs(dy) < 1e-3f) dy = 1;
        const float t = std::min((cx - inset) / std::max(std::abs(dx), 1e-3f), (cy - inset) / std::max(std::abs(dy), 1e-3f));
        return {cx + dx * t, cy + dy * t, std::atan2(dy, dx), true};
    }
}

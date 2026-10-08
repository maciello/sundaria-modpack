#pragma once
// SDK-free logic of the dungeon map (#40): the main path (the game's navmesh path, world units), the local player's
// frontier on it, the drawn part, the flow pulses and the world → minimap transform. Spec: design-system.md § Dungeon map path.
#include <algorithm>
#include <cmath>
#include <vector>

namespace dungeon_map {
    struct V3 { float x = 0, y = 0, z = 0; };
    inline float Dist(V3 a, V3 b) { return std::sqrt((a.x - b.x) * (a.x - b.x) + (a.y - b.y) * (a.y - b.y) + (a.z - b.z) * (a.z - b.z)); }
    using Path = std::vector<V3>;

    constexpr float kReach = 600;     // world units (6 m): coming this close to the path counts as being there
    constexpr float kStep = 100;      // frontier moves in steps of at least this (fewer redraws)
    constexpr float kPartial = 300;   // path end this far from the goal = the navmesh stopped (closed door)

    inline float Length(const Path& p) {
        float l = 0;
        for (size_t i = 1; i < p.size(); i++) l += Dist(p[i - 1], p[i]);
        return l;
    }
    // Nearest point of the path to q: its distance along the path (s) and from q (d).
    struct Proj { float s = 0, d = INFINITY; };
    inline Proj Project(const Path& p, V3 q) {
        Proj best;
        if (p.size() == 1) return {0, Dist(p[0], q)};
        float s0 = 0;
        for (size_t i = 1; i < p.size(); i++) {
            const V3 a = p[i - 1], b = p[i];
            const float len = Dist(a, b);
            float t = 0;
            if (len > 0) t = std::clamp(((q.x - a.x) * (b.x - a.x) + (q.y - a.y) * (b.y - a.y) + (q.z - a.z) * (b.z - a.z)) / (len * len), 0.f, 1.f);
            const V3 c{a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t, a.z + (b.z - a.z) * t};
            if (const float d = Dist(c, q); d < best.d) best = {s0 + len * t, d};
            s0 += len;
        }
        return best;
    }
    inline V3 At(const Path& p, float s) {
        if (p.empty()) return {};
        for (size_t i = 1; i < p.size(); i++) {
            const float len = Dist(p[i - 1], p[i]);
            if (s <= len && len > 0) {
                const float t = std::max(s, 0.f) / len;
                return {p[i - 1].x + (p[i].x - p[i - 1].x) * t, p[i - 1].y + (p[i].y - p[i - 1].y) * t, p[i - 1].z + (p[i].z - p[i - 1].z) * t};
            }
            s -= len;
        }
        return p.back();
    }
    // The path from its start to distance s (the drawn line).
    inline Path Prefix(const Path& p, float s) {
        Path out;
        if (p.empty() || s < 0) return out;
        out.push_back(p[0]);
        float s0 = 0;
        for (size_t i = 1; i < p.size(); i++) {
            const float len = Dist(p[i - 1], p[i]);
            if (s0 + len >= s) break;
            out.push_back(p[i]);
            s0 += len;
        }
        out.push_back(At(p, s));
        return out;
    }

    // The local player's frontier on one floor: a place, the furthest point of the path they came within kReach of.
    // Kept as a world point, so a recomputed path (a door opened) keeps it where it was. Never moves back.
    struct Frontier {
        bool has = false;
        V3 at;
        float S(const Path& p) const { return has ? Project(p, at).s : -1; }
        bool Visit(const Path& p, V3 pawn) {  // true = advanced
            if (p.empty()) return false;
            const Proj q = Project(p, pawn);
            if (q.d > kReach || (has && q.s < S(p) + kStep)) return false;
            at = At(p, q.s);
            has = true;
            return true;
        }
    };

    // World → minimap pixel (in game, game-ui.md § Minimap): (Y, -X) / UnitToPixel.
    struct Px { float x = 0, y = 0; };
    inline Px ToMap(V3 w, float unitToPixel) { return {w.y / unitToPixel, -w.x / unitToPixel}; }

    // Flow pulses along a drawn line of lenPx map pixels at time t (seconds): position (px from the start) and opacity.
    constexpr float kPulseSpeed = 40, kPulseGap = 36, kPulseFade = 16;
    constexpr int kMaxPulses = 24;
    struct Pulse { float s, alpha; };
    inline std::vector<Pulse> Pulses(float lenPx, double t) {
        std::vector<Pulse> out;
        if (lenPx < kPulseGap * 0.5f) return out;
        const int n = std::clamp(int(lenPx / kPulseGap), 1, kMaxPulses);
        const float gap = lenPx / n;  // n pulses spread over the line, looping
        const float head = float(std::fmod(t * kPulseSpeed, double(gap)));
        for (int i = 0; i < n; i++) {
            const float s = head + i * gap;
            out.push_back({s, std::clamp(std::min(s, lenPx - s) / kPulseFade, 0.f, 1.f)});
        }
        return out;
    }

    // A room's box (centre, half extents, yaw in degrees around Z): is the pawn inside?
    struct Box { V3 c, e; float yawDeg = 0; };
    inline bool Inside(const Box& b, V3 p) {
        const float a = -b.yawDeg * 3.14159265f / 180.f, dx = p.x - b.c.x, dy = p.y - b.c.y;
        const float lx = dx * std::cos(a) - dy * std::sin(a), ly = dx * std::sin(a) + dy * std::cos(a);
        return std::abs(lx) <= b.e.x && std::abs(ly) <= b.e.y && std::abs(p.z - b.c.z) <= b.e.z;
    }
}

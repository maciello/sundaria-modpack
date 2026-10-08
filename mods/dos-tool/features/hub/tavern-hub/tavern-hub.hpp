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

    struct Spot { std::string name; float x, y, z, yaw; };

    inline std::vector<Spot> ParseLayout(const std::string& text) {
        std::vector<Spot> out;
        std::istringstream in(text);
        std::string line;
        while (std::getline(in, line)) {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            const size_t eq = line.find('=');
            if (line.empty() || line[0] == '#' || eq == std::string::npos || eq == 0) continue;
            Spot s{line.substr(0, eq), 0, 0, 0, 0};
            if (std::sscanf(line.c_str() + eq + 1, "%f,%f,%f,%f", &s.x, &s.y, &s.z, &s.yaw) != 4) continue;
            for (Spot& o : out) if (o.name == s.name) { o = s; s.name.clear(); }  // a repeated name: last line wins
            if (!s.name.empty()) out.push_back(s);
        }
        return out;
    }

    inline std::string WriteLayout(const std::vector<Spot>& spots) {
        std::string out = "# tavern hub: NPC=x,y,z,yaw (written by dos-tool)\n";
        char buf[256];
        for (const Spot& s : spots) {
            std::snprintf(buf, sizeof(buf), "%s=%.1f,%.1f,%.1f,%.1f\n", s.name.c_str(), s.x, s.y, s.z, s.yaw);
            out += buf;
        }
        return out;
    }

    // Insert or replace the spot for s.name.
    inline void SetSpot(std::vector<Spot>& spots, const Spot& s) {
        for (Spot& o : spots) if (o.name == s.name) { o = s; return; }
        spots.push_back(s);
    }
}

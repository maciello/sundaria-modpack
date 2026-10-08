#pragma once
#include <cmath>
#include <cstdint>
#include <unordered_map>
#include <vector>

// SDK-free damage-number bookkeeping: diff per-actor health between samples,
// emit a floating number for every change. Drawing lives in overlay.cpp.
namespace dmgnum {
    struct Sample { uintptr_t id; float x, y, z; float health; };
    struct Number { float x, y, z; float amount; double born; };  // amount > 0 = damage, < 0 = heal

    // UE camera POV (cm, degrees; FOV horizontal).
    struct View { float x, y, z, pitch, yaw, roll, fov; };

    // World point -> screen pixels, UE axis conventions (X fwd, Y right, Z up). false if behind camera.
    inline bool Project(const View& v, float px, float py, float pz, float w, float h, float& sx, float& sy) {
        constexpr float d2r = 3.14159265f / 180.0f;
        const float sp = std::sin(v.pitch * d2r), cp = std::cos(v.pitch * d2r);
        const float sy_ = std::sin(v.yaw * d2r), cy = std::cos(v.yaw * d2r);
        const float sr = std::sin(v.roll * d2r), cr = std::cos(v.roll * d2r);
        const float ax[3] = {cp * cy, cp * sy_, sp};
        const float ay[3] = {sr * sp * cy - cr * sy_, sr * sp * sy_ + cr * cy, -sr * cp};
        const float az[3] = {-(cr * sp * cy + sr * sy_), cy * sr - cr * sp * sy_, cr * cp};
        const float d[3] = {px - v.x, py - v.y, pz - v.z};
        const float right = d[0] * ay[0] + d[1] * ay[1] + d[2] * ay[2];
        const float up = d[0] * az[0] + d[1] * az[1] + d[2] * az[2];
        const float fwd = d[0] * ax[0] + d[1] * ax[1] + d[2] * ax[2];
        if (fwd < 1.0f) return false;
        const float cx = w * 0.5f, cyy = h * 0.5f;
        const float f = cx / std::tan(v.fov * d2r * 0.5f);
        sx = cx + right * f / fwd;
        sy = cyy - up * f / fwd;
        return true;
    }

    struct Tracker {
        double lifetime = 1.2;  // seconds on screen
        std::unordered_map<uintptr_t, float> last;
        std::vector<Number> live;

        void Update(const std::vector<Sample>& samples, double now) {
            std::unordered_map<uintptr_t, float> seen;
            for (const Sample& s : samples) {
                seen[s.id] = s.health;
                auto it = last.find(s.id);
                if (it != last.end() && s.health != it->second)
                    live.push_back({s.x, s.y, s.z, it->second - s.health, now});
            }
            last.swap(seen);  // actors that vanished are forgotten (no number on despawn)
            std::erase_if(live, [&](const Number& n) { return now - n.born > lifetime; });
        }
    };
}

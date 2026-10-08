#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <unordered_map>
#include <vector>

// SDK-free damage-number bookkeeping: diff per-actor health between samples,
// emit a floating number for every change, keep fight stats. Drawing lives in overlay.cpp.
namespace dmgnum {
    struct Sample { uintptr_t id; float x, y, z; float health; bool isPlayer; };

    enum class Kind { Dealt, Taken, Heal };
    struct Number {
        float x, y, z;
        float amount;  // always > 0
        Kind kind;
        float scale;   // relative size: 1 = your typical hit
        float drift;   // -1..1 sideways direction
        double born;
    };

    // Fight = damage dealt to non-players with no gap longer than `gap` seconds.
    struct Fight {
        double start = 0, last = 0;
        double total = 0;
        double Duration() const { return std::max(1.0, last - start); }
        double Dps() const { return total / Duration(); }
    };

    struct Tracker {
        double lifetime = 1.4;   // seconds on screen
        double grace = 1.5;      // ignore changes this long after first sight (spawn HP fill-up)
        double gap = 5.0;        // fight ends after this long without damage dealt
        float typical = 0;       // EMA of hit size (dealt), the "1.0" for scale

        std::unordered_map<uintptr_t, float> last;
        std::unordered_map<uintptr_t, double> firstSeen;
        std::vector<Number> live;
        Fight fight;
        bool inFight = false;

        float Scale(float amount) {
            if (typical <= 0) typical = amount;
            const float s = 1.0f + 0.45f * std::log2(amount / typical);
            typical += 0.08f * (amount - typical);
            return std::clamp(s, 0.6f, 2.2f);
        }

        void Update(const std::vector<Sample>& samples, double now) {
            std::unordered_map<uintptr_t, float> seen;
            std::unordered_map<uintptr_t, double> first;
            for (const Sample& s : samples) {
                seen[s.id] = s.health;
                auto fs = firstSeen.find(s.id);
                first[s.id] = fs != firstSeen.end() ? fs->second : now;
                auto it = last.find(s.id);
                if (it == last.end() || s.health == it->second) continue;
                if (now - first[s.id] < grace || it->second <= 0) continue;
                const float delta = it->second - s.health;
                const float drift = float((s.id >> 4) % 200) / 100.0f - 1.0f;
                if (delta < 0) {
                    live.push_back({s.x, s.y, s.z, -delta, Kind::Heal, 0.8f, drift, now});
                } else if (s.isPlayer) {
                    live.push_back({s.x, s.y, s.z, delta, Kind::Taken, 0.9f, drift, now});
                } else {
                    live.push_back({s.x, s.y, s.z, delta, Kind::Dealt, Scale(delta), drift, now});
                    if (!inFight || now - fight.last > gap) { fight = {now, now, 0}; inFight = true; }
                    fight.total += delta;
                    fight.last = now;
                }
            }
            last.swap(seen);  // actors that vanished are forgotten (no number on despawn)
            firstSeen.swap(first);
            std::erase_if(live, [&](const Number& n) { return now - n.born > lifetime; });
        }

        bool FightActive(double now) const { return inFight && now - fight.last <= gap; }
    };

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

    // Genshin-ish pop: overshoot to 1.25x, settle to 1.0 by 0.25 s, shrink a bit while fading.
    inline float PopScale(double age) {
        if (age < 0.08) return float(0.5 + (1.25 - 0.5) * age / 0.08);
        if (age < 0.25) return float(1.25 - 0.25 * (age - 0.08) / 0.17);
        return 1.0f;
    }
}

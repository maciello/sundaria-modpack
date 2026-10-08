#pragma once
#include <cstdint>
#include <unordered_map>
#include <vector>

// SDK-free damage-number bookkeeping: diff per-actor health between samples,
// emit a floating number for every change. Drawing lives in overlay.cpp.
namespace dmgnum {
    struct Sample { uintptr_t id; float x, y, z; float health; };
    struct Number { float x, y, z; float amount; double born; };  // amount > 0 = damage, < 0 = heal

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

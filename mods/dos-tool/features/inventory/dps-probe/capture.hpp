#pragma once
#include <cstdint>
#include <unordered_map>

// DPS probe hit capture (#111): which of the hero's hits become records. Per enemy class the first non-crit hit
// and the first crit, at most kMaxClasses classes. SDK-free.
namespace dps_probe {
    struct Keep {
        static constexpr int kMaxClasses = 32;
        std::unordered_map<uint64_t, uint8_t> seen;  // class key -> bit 0 hit kept, bit 1 crit kept

        bool NewClass(uint64_t cls) const { return !seen.count(cls); }
        // true = record this hit (and remember it)
        bool Take(uint64_t cls, bool crit) {
            const uint8_t bit = crit ? 2 : 1;
            auto it = seen.find(cls);
            if (it == seen.end()) {
                if (int(seen.size()) >= kMaxClasses) return false;
                seen[cls] = bit;
                return true;
            }
            if (it->second & bit) return false;
            it->second |= bit;
            return true;
        }
    };
}

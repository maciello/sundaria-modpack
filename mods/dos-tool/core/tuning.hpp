#pragma once
// Values one feature applies to the game and others must model (features do not include each other).
#include <atomic>

namespace tuning {
    // Dual-wield damage's k while that feature is on and writing, else 0. Set by dual-wield-damage, read by item-upgrade (DPS model).
    inline std::atomic<float> dualWieldK{0.0f};
}

#pragma once
// Expected-value DPS of one attribute row: damage per cast per ability (formula.hpp), then the rotation.
#include "dps/c_api.h"

namespace dps::kernel {
    struct Detail { float dmg[32], cast[32], cd[32], uses[32]; };   // per ability (layout::kMaxAbilities)
    // x: attribute row (layout.hpp, c.A floats). detail: optional.
    float Eval(const dps_ctx& c, const float* x, Detail* detail = nullptr);
    // rows b0..b1 of a batch (attrs rows, or assembled from gear); out[b], per_ability[b][K] optional
    void EvalRange(int b0, int b1, const float* attrs, const dps_gear* g, const dps_ctx& c, float* out, float* per_ability);
}

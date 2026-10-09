#include "kernel.hpp"
#include "formula.hpp"
#include "layout.hpp"
#include "rotation.hpp"
#include <algorithm>
#include <cstddef>

namespace dps::kernel {
    using namespace layout;
    namespace f = formula;

    float Eval(const dps_ctx& c, const float* x, Detail* detail) {
        const int K = c.K;
        const float* s = c.scen;
        const float L = s[S_L], Lt = s[S_LT], T = s[S_T];
        float ap[3];
        for (int i = 0; i < 3; ++i) ap[i] = f::AttackPower(x[I_AP + i], x[I_APB + i]);
        const float base = x[I_WD] * (1.f + x[I_DPHYS]);
        const float crit = f::CritChance(x[I_CC], x[I_BCB]);
        const float hitPhys = f::ExpectedHit(crit, x[I_CD], f::CritCut(s[S_GLANCE], Lt));
        const float hitMag = f::ExpectedHit(crit, x[I_CD], f::CritCut(s[S_DEFLECT], Lt));
        const float armPhys = f::Mitigation(x[I_APEN], s[S_ARMOR], L, Lt), armMag = f::Mitigation(x[I_MPEN], s[S_MR], L, Lt);
        const float rate = 1.f + x[I_IAS];
        const float cdm = std::max(1.f - x[I_CDR], 0.01f);
        const int we = std::clamp((int)x[I_WELIDX], 0, kElements - 1);
        // weapon elemental clone: base = WeaponDamage_<first element>, magic of that element (Hit = 1 assumed)
        const float wel = x[I_WEL] * f::DamageByType(x[I_DEL + we]) * f::Resist(s[S_INC], s[S_RESEL + we]) * armMag;

        Detail local;
        Detail& d = detail ? *detail : local;
        for (int k = 0; k < K; ++k) {
            d.cast[k] = std::max(c.ab_cast[k] / rate, 0.05f);
            d.cd[k] = c.ab_cd[k] * cdm;
            d.dmg[k] = 0.f;
        }
        for (int j = 0; j < c.C; ++j) {
            const int k = c.c_ab[j], e = c.c_elem[j];
            const bool mag = c.c_magic[j];
            const float abil = (c.c_coef[j] + x[I_AB + k]) * (1.f + x[I_AB + K + k]);
            const float dt = mag ? f::DamageByType(x[I_DEL + e]) : f::DamageByType(x[I_OUTPHYS]);
            const float rs = f::Resist(s[S_INC], mag ? s[S_RESEL + e] : s[S_RESPHYS]);
            const float a = ap[c.c_ap[j]];
            const float perHit = base * (mag ? hitMag : hitPhys) * a * dt * rs * (mag ? armMag : armPhys) * abil;
            float mult = c.c_hits[j] * c.c_targets[j];
            if (c.c_dot_per[j] > 0.f) {   // DoT: one application per cast, ticks over min(duration, max(cd, cast))
                mult *= std::min(c.c_dot_dur[j], std::max(d.cd[k], d.cast[k])) / c.c_dot_per[j];
                d.dmg[k] += perHit * mult;
            } else {
                d.dmg[k] += (perHit + wel * a * abil) * mult;
            }
        }
        return rotation::Run(K, d.dmg, d.cast, d.cd, T, s[S_MODE] != 0.f, d.uses) / T;
    }

    void EvalRange(int b0, int b1, const float* attrs, const dps_gear* g, const dps_ctx& c, float* out, float* per_ability) {
        const int A = c.A, K = c.K;
        float buf[I_AB + 2 * kMaxAbilities];
        Detail d;
        for (int b = b0; b < b1; ++b) {
            const float* x = attrs ? attrs + (size_t)b * A : buf;
            if (!attrs) {
                for (int i = 0; i < A; ++i) buf[i] = g->base[i];
                for (int q = 0; q < g->S; ++q) {
                    const float* r = g->rows + (size_t)(g->slot_off[q] + g->choice[(size_t)b * g->S + q]) * A;
                    for (int i = 0; i < A; ++i) buf[i] += r[i];
                }
                for (int h = 0; h < g->H; ++h) {
                    const float p = g->pts[(size_t)b * g->H + h];
                    if (p != 0.f) for (int i = 0; i < A; ++i) buf[i] += p * g->Hm[(size_t)h * A + i];
                }
            }
            out[b] = Eval(c, x, per_ability ? &d : nullptr);
            if (per_ability)
                for (int k = 0; k < K; ++k) per_ability[(size_t)b * K + k] = d.dmg[k] * d.uses[k] / c.scen[S_T];
        }
    }
}

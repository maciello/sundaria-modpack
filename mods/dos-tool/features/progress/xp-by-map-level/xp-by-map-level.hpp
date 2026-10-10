#pragma once
#include <cmath>
// SDK-free curve for XP by map level (#134). d = map level - hero level.
// m = 1 + (max-1) * n(d; upC, upW)   for d >= 0
// m = min ^ n(-d; loC, loW)          for d < 0      n(x;c,w) = (sig((x-c)/w) - sig(-c/w)) / (1 - sig(-c/w))
namespace xp_by_map_level {
    struct Curve { float maxMult = 5.0f, minMult = 0.05f, upC = 55.0f, upW = 15.0f, loC = 20.0f, loW = 6.0f; };

    inline double Sig(double x) { return 1.0 / (1.0 + std::exp(-x)); }
    inline double N(double x, double c, double w) {
        const double s0 = Sig(-c / w);
        return (Sig((x - c) / w) - s0) / (1.0 - s0);
    }

    inline double Mult(int d, const Curve& k) {
        if (d >= 0) return 1.0 + (k.maxMult - 1.0) * N(d, k.upC, k.upW);
        return std::pow(double(k.minMult), N(-d, k.loC, k.loW));
    }

    // Extra XP to grant on top of `gained`; 0 when the multiplier is <= 1 (we never take XP away).
    inline int Extra(int gained, double m) {
        return (gained > 0 && m > 1.0) ? int(std::floor((m - 1.0) * gained + 0.5)) : 0;
    }
}

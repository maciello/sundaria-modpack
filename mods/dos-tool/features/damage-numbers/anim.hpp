#pragma once
#include <algorithm>
#include <cmath>

namespace dmgnum {
    // Motion-graphics curve for one number. Offsets are in units of the number's base size.
    struct Anim { float dx, dy, scale, alpha, flash; };

    inline float EaseOutCubic(float x) { const float u = 1 - x; return 1 - u * u * u; }
    inline float EaseInQuad(float x) { return x * x; }
    inline float EaseOutBack(float x, float k) {
        const float c1 = k, c3 = c1 + 1, u = x - 1;
        return 1 + c3 * u * u * u + c1 * u * u;
    }

    // sinceBorn: first hit; sinceBump: last merged hit. big: 0 = typical, 1 = huge.
    inline Anim Animate(double sinceBorn, double sinceBump, double lifetime, float drift, float big, bool stacked) {
        const float age = float(sinceBorn), bump = float(sinceBump);
        const float pop = std::min(age / 0.22f, 1.0f);
        const float rise = std::min(age / 0.6f, 1.0f);
        const float out = std::clamp(float(sinceBump / lifetime - 0.7) / 0.3f, 0.0f, 1.0f);
        const float kick = stacked ? 0.35f * EaseInQuad(1.0f - std::min(bump / 0.25f, 1.0f)) : 0.0f;
        Anim a;
        a.scale = EaseOutBack(pop, 1.7f + 1.6f * big) * (1.0f + kick) * (1.0f - 0.25f * EaseInQuad(out));
        a.dx = drift * 0.9f * EaseOutCubic(rise);
        a.dy = -1.6f * EaseOutCubic(rise) - 0.5f * EaseInQuad(out);
        a.alpha = 1.0f - EaseInQuad(out);
        a.flash = 1.0f - std::min(std::min(age, bump) / (0.12f + 0.1f * big), 1.0f);
        return a;
    }
}

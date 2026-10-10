#pragma once
#include <cmath>
#include <cstdio>
#include <string>

// Training ally (#125) logic, SDK-free: which stats are shown above the adopted dummy, how a change reads, and which
// character near the remembered spot is the dummy.
namespace training_ally {
    // The stats the buff catalogue of #106 moves, in display order. `pct`: the game stores a fraction or a percent
    // value we show with a % sign (unverified which; the number is shown as stored).
    struct Row { const char* label; bool pct; };
    constexpr Row kRows[] = {
        {"Health", false},        {"Melee power", false},   {"Ranged power", false},  {"Spell power", false},
        {"Crit chance", true},    {"Crit dmg melee", true}, {"Crit dmg ranged", true}, {"Crit dmg spell", true},
        {"Attack speed", true},   {"Move speed", true},     {"Damage dealt", true},   {"Damage taken", true},
        {"Crit dmg dealt", true}, {"Cooldowns x", false},
    };
    constexpr int kRowCount = int(sizeof(kRows) / sizeof(kRows[0]));
    enum { Health, Melee, Ranged, Spell, Crit, CritMelee, CritRanged, CritSpell, AttackSpeed, MoveSpeed, DamageOut, DamageIn, CritOut, Cooldown };

    // A stat counts as changed when it moved by more than rounding noise.
    inline bool Changed(float base, float now) {
        const float d = std::fabs(now - base);
        return d > 0.005f && d > std::fabs(base) * 0.0005f;
    }

    inline std::string Num(float v) {
        char b[32];
        const float a = std::fabs(v);
        if (a >= 1e6f) std::snprintf(b, sizeof(b), "%.1fM", v / 1e6f);
        else if (a >= 1e4f) std::snprintf(b, sizeof(b), "%.1fk", v / 1e3f);
        else if (a >= 100 || v == std::floor(v)) std::snprintf(b, sizeof(b), "%.0f", v);
        else std::snprintf(b, sizeof(b), "%.2f", v);
        return b;
    }

    // "Crit chance 5 -> 15 (+10)" when changed, "Crit chance 5" else.
    inline std::string Line(const Row& r, float base, float now) {
        std::string s = std::string(r.label) + " " + Num(Changed(base, now) ? base : now) + (r.pct ? "%" : "");
        if (!Changed(base, now)) return s;
        const float d = now - base;
        return s + " -> " + Num(now) + (r.pct ? "%" : "") + " (" + (d > 0 ? "+" : "") + Num(d) + ")";
    }

    // "MovementSpeedUp 12s" / "Regen" (no end).
    inline std::string Effect(std::string cls, float remaining) {
        for (const char* pre : {"GE_", "BP_GameplayEffect_"})
            if (cls.rfind(pre, 0) == 0) cls.erase(0, std::char_traits<char>::length(pre));
        if (cls.size() > 2 && cls.compare(cls.size() - 2, 2, "_C") == 0) cls.erase(cls.size() - 2);
        if (remaining < 0) return cls;
        char b[16];
        std::snprintf(b, sizeof(b), " %.0fs", remaining);
        return cls + b;
    }

    // The remembered dummy is the character closest to its spot, if within `radius` (dummies do not walk).
    inline bool Nearer(float dx, float dy, float dz, float radius, float& best) {
        const float d = std::sqrt(dx * dx + dy * dy + dz * dz);
        if (d > radius || d >= best) return false;
        best = d;
        return true;
    }
}

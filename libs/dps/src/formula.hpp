#pragma once
// One hit, expected value (game-facts.md dps_mechanics; pak: CustomCalculation_Damage, ApplyTransientDamageInfoGE).
// Damage = BaseDamage x Hit x AP x DmgType x Resist x Armor x Ability.
#include <algorithm>

namespace dps::formula {
    inline float Clamp01(float x) { return x < 0.f ? 0.f : (x > 1.f ? 1.f : x); }
    // AP term: attack power x (1 + <X>Power_Bonus) / 220
    inline float AttackPower(float ap, float bonus) { return ap * (1.f + bonus) / 220.f; }
    // crit chance (0.05 + CriticalChance) x (1 + BaseCritChance_Bonus)
    inline float CritChance(float critChance, float baseBonus) { return Clamp01((0.05f + critChance) * (1.f + baseBonus)); }
    // target's glancing blow (physical) / deflect (magic) cuts the crit multiplier
    inline float CritCut(float g, float targetLevel) { return Clamp01(g / (g + 400.f + 10.f * targetLevel)); }
    // expected Hit: no crit 1, crit (1.5 + CriticalDamage_Melee) x (1 - cut); blocks not modelled
    inline float ExpectedHit(float crit, float critDamage, float cut) { return (1.f - crit) + crit * (1.5f + critDamage) * (1.f - cut); }
    // expected Armor: penetration chance P/(P+400+10L) ignores armor, else 1 - A/(A+700+4Lt)
    inline float Mitigation(float pen, float armor, float level, float targetLevel) {
        const float p = pen / (pen + 400.f + 10.f * level);
        return p + (1.f - p) * (1.f - Clamp01(armor / (armor + 700.f + 4.f * targetLevel)));
    }
    inline float DamageByType(float bonus) { return std::max(0.f, 1.f + bonus); }                     // attacker
    inline float Resist(float incoming, float resist) { return std::max(0.f, 1.f + incoming - resist); } // target
}

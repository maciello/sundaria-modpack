#pragma once
// SDK-free logic for Dual-wield damage (#136): the game reads ONE weapon's WeaponDamage per cast and alternates hands,
// so two one-handers deal one weapon per cast. Each hand's WeaponDamage becomes own + k * other (skill game-facts
// dps_mechanics.weapon_damage_hands).
namespace dual_wield_damage {
    constexpr float kDefault = 0.85f;  // parity with a 2H weapon (2H carries ~1.9x a 1H WeaponDamage)
    constexpr float kMax = 1.5f;

    // left/right: the hand's active weapon attribute set exists. same: both hands resolve to one set (a 2H weapon).
    struct Hands { bool left, right, same; float dl, dr; };
    struct Out { bool apply; float l, r; };

    // Dual wield = two distinct weapon sets that both deal damage (a shield or an empty hand reads 0).
    inline bool DualWields(const Hands& h) { return h.left && h.right && !h.same && h.dl > 0.0f && h.dr > 0.0f; }

    // Values to write; apply=false leaves both hands vanilla.
    inline Out Plan(const Hands& h, float k) {
        if (!DualWields(h) || !(k > 0.0f)) return {false, h.dl, h.dr};
        return {true, h.dl + k * h.dr, h.dr + k * h.dl};
    }
}

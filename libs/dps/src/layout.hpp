#pragma once
// Attribute row and scenario layout shared by compile.cpp and the kernel.
namespace dps::layout {
    // attrs: AP[3] (MAP RAP SP), AP bonus[3], weapon damage, physical damage bonus (Damage_<weapon damage type)>,
    // elemental damage bonus[8], physical out (heroism), crit chance, base crit bonus, crit damage, armor pen, magic pen,
    // attack speed, cooldown reduction, weapon element value + index, then per ability: bonus coef [K], heroism mult [K]
    enum { I_AP = 0, I_APB = 3, I_WD = 6, I_DPHYS = 7, I_DEL = 8, I_OUTPHYS = 16, I_CC = 17, I_BCB = 18, I_CD = 19,
           I_APEN = 20, I_MPEN = 21, I_IAS = 22, I_CDR = 23, I_WEL = 24, I_WELIDX = 25, I_AB = 26 };
    // scen: level, target level, armor, magic resist, glancing, deflect, incoming damage mod, physical resist, fight s,
    // elemental resist[8], rotation mode (0 fluid, 1 event)
    enum { S_L, S_LT, S_ARMOR, S_MR, S_GLANCE, S_DEFLECT, S_INC, S_RESPHYS, S_T, S_RESEL, S_MODE = S_RESEL + 8, S_N };
    inline constexpr int kMaxAbilities = 32;
    inline constexpr int kElements = 8;
}

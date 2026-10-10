#pragma once
// Class x weapon x ability levels x scenario -> flat arrays for the kernel (dps_ctx) + how item stats land in a row.
#include "dps/c_api.h"
#include "dps/dps.hpp"
#include "layout.hpp"
#include <array>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace dps::compile {
    struct Params {
        std::string cls;
        std::string weapon;      // weapon type as the spec names it (Weapon_Stats row), e.g. HeavyCrossbow
        std::string offhand;     // animation type in the other hand ("" = none / two-handed)
        int level = 1;
        std::array<float, 6> primary{};
        const std::vector<std::pair<std::string, int>>* learned = nullptr;   // null/empty = all abilities at abilityLevel
        int abilityLevel = 3;
        const std::vector<std::pair<std::string, int>>* heroism = nullptr;
        const Scenario* scenario = nullptr;
    };
    struct AbilityInfo { std::string name, montage, how; int level = 0; };

    struct Compiled {
        std::string cls, weapon, animType, damageType, offhand;
        int A = 0, K = 0;
        std::vector<AbilityInfo> abilities;
        std::vector<std::string> dropped;
        std::vector<float> base;                       // [A]: attack power from primaries + level, heroism
        std::unordered_map<std::string, int> statAttr; // lowercase stat name -> attribute index
        std::vector<float> abCast, abCd, cCoef, cHits, cTargets, cDotDur, cDotPer, cWd;
        std::vector<int32_t> cAb, cAp, cMagic, cElem;
        std::array<float, layout::S_N> scen{};
        dps_ctx Ctx() const;
        // stat -> attribute index, or element 0..7 for WeaponDamage_<element> as -(element + 2); -1 = no reader
        int Attr(const std::string& lowerName) const;
    };

    // error non-empty = nothing compiled (unknown class or weapon, no damaging ability)
    Compiled Compile(const Tables& t, const Params& p, std::string& error);
    std::string Lower(std::string s);
    int ElementIndex(const std::string& name);   // Fire..Light -> 0..7, -1 = none
}

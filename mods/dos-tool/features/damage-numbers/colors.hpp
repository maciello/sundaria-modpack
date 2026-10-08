#pragma once
#include <string_view>

// Damage-type class name (UBP_DamageType_Magic_Ice_C, UBP_DamageDOT_Poison_C, …) → element → number colour.
// First keyword match wins, so specific elements are listed before the generic "Magic".
namespace dmgnum {
    enum class Element { Physical, Fire, Ice, Lightning, Holy, Poison, Shadow, Arcane, Environment };
    struct Rgb { float r, g, b; };

    inline Element Classify(std::string_view type) {
        struct Rule { std::string_view key; Element e; };
        static constexpr Rule rules[] = {
            {"Environment", Element::Environment}, {"Environmental", Element::Environment},
            {"Poison", Element::Poison}, {"Toxic", Element::Poison}, {"Natural", Element::Poison},
            {"Burn", Element::Fire}, {"Fire", Element::Fire}, {"Flame", Element::Fire}, {"Ember", Element::Fire},
            {"Meteor", Element::Fire}, {"Brand", Element::Fire},
            {"Ice", Element::Ice},
            {"Lightning", Element::Lightning},
            {"Holy", Element::Holy}, {"Light", Element::Holy}, {"Smite", Element::Holy}, {"Heavenly", Element::Holy},
            {"Death", Element::Shadow}, {"Void", Element::Shadow}, {"Vampiric", Element::Shadow}, {"Demon", Element::Shadow},
            {"Magic", Element::Arcane},
        };
        for (const Rule& r : rules)
            if (type.find(r.key) != std::string_view::npos) return r.e;
        return Element::Physical;
    }

    // Physical keeps the white→gold "size" ramp; elements get a fixed hue.
    inline Rgb ColorOf(Element e) {
        switch (e) {
            case Element::Fire:        return {255, 140, 40};
            case Element::Ice:         return {130, 210, 255};
            case Element::Lightning:   return {185, 130, 255};
            case Element::Holy:        return {255, 230, 140};
            case Element::Poison:      return {170, 220, 50};
            case Element::Shadow:      return {230, 80, 200};
            case Element::Arcane:      return {110, 160, 255};
            case Element::Environment: return {190, 190, 190};
            default:                   return {255, 255, 255};
        }
    }
}

#pragma once
#include <string_view>

// Damage-type class name (BP_DamageType_Magic_Ice_C, BP_DamageDOT_Poison_C, …) → element.
// First keyword match wins, so specific elements are listed before the generic "Magic".
namespace combat {
    enum class Element : unsigned char { Physical, Fire, Ice, Lightning, Holy, Poison, Shadow, Arcane, Environment };

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
}

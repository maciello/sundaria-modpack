// just test
#include "colors.hpp"
#include <cassert>
#include <cstdio>

using dmgnum::Classify; using dmgnum::Element;

int main() {
    assert(Classify("BP_DamageType_Magic_Ice_C") == Element::Ice);
    assert(Classify("BP_DamageType_Magic_Fireball_C") == Element::Fire);        // fire before generic Magic
    assert(Classify("BP_DamageType_Magic_HolyLight_C") == Element::Holy);
    assert(Classify("BP_DamageType_Magic_Lightning_C") == Element::Lightning);  // not Holy via "Light"
    assert(Classify("BP_DamageType_Magic_Poison_C") == Element::Poison);
    assert(Classify("BP_DamageType_Range_ToxicArrow_C") == Element::Poison);
    assert(Classify("BP_DamageDOT_Brand_SearingTrap_C") == Element::Fire);
    assert(Classify("BP_DamageType_Magic_Void_C") == Element::Shadow);
    assert(Classify("BP_DamageType_Melee_VampiricBlades_C") == Element::Shadow);
    assert(Classify("BP_DamageType_Magic_MagicMissile_C") == Element::Arcane);
    assert(Classify("BP_DamageType_Environment_Trap_C") == Element::Environment);
    assert(Classify("BP_DamageType_Melee_Backstab_C") == Element::Physical);
    assert(Classify("") == Element::Physical);
    std::puts("ok");
}

// just test
#include "items.hpp"
#include <cassert>
#include <cstdio>

using namespace items;

int main() {
    const std::vector<std::string> names = {"Melee Damage", "Ranged Damage", "Armor"};
    assert(AttackOfWeapon("Wand") == Attack::Magic);
    assert(AttackRank(Attack::Ranged, Attack::Melee) == 2 && AttackRank(Attack::Melee, Attack::Ranged) == 2);
    assert(StatName(names, 9) == "Stat 9");
    // ranged/melee rule (weapon type names as logged in game)
    for (const char* m : {"Axe", "Club", "Dagger", "Fist", "Sword", "Shield", "Axe2H", "Hammer2H"}) assert(AttackOfWeapon(m) == Attack::Melee);
    assert(AttackOfWeapon("Crossbow") == Attack::Ranged && AttackOfWeapon("Bow2H") == Attack::Ranged && AttackOfWeapon("Crossbow2H") == Attack::Ranged);
    assert(AttackOfWeapon("Staff2H") == Attack::Magic && AttackOfWeapon("") == Attack::Unknown);
    assert(AttackIn("MeleePower_Bonus") == Attack::Melee && AttackIn("RAP") == Attack::Ranged && AttackIn("CriticalDamage_Spell") == Attack::Magic);
    assert(AttackIn("Damage_Slash") == Attack::Unknown);
    assert(WhereOf("Equipment", false) == Where::Equipped);
    assert(WhereOf("MainInventory", false) == Where::Inventory);
    assert(WhereOf("Equipment", true) == Where::Bank);
    // weapons weigh their own attack stats, armor the profile's focus; a player override wins
    Item bow; bow.kind = Kind::Weapon; bow.attack = Attack::Ranged;
    Item sword = bow; sword.attack = Attack::Melee;
    Item helm; helm.kind = Kind::Armor;
    const auto pre = Presets();
    Profile p{"Custom", Attack::Ranged};
    p.weight["RAP"] = 0.5f;
    assert(WeightFor(p, bow, "RAP") == 0.5f && WeightFor(pre[2], sword, "Map") == 2.0f && WeightFor(pre[2], helm, "Map") == 0.0f);
    // buckets: weapons by attack rank then type, armor by equip slot, groups first
    sword.weaponType = 2; helm.equipSlot = 4;
    assert(BucketOf(bow, Attack::Ranged) < BucketOf(sword, Attack::Ranged) && BucketOf(sword, Attack::Ranged) < BucketOf(helm, Attack::Ranged));
    Item ring; ring.equipSlot = 9;
    Item potion;
    assert(Group(ring) == 2 && Group(potion) == 3);
    std::puts("ok");
}

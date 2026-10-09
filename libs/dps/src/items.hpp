#pragma once
// Item generation, scalable crafting-bonus path (EnableRandomStats 6/8, nearly every item; game-facts.md
// dps_mechanics.item_affixes): which stats can roll on an item and the exact value of each.
#include "dps/tables.hpp"
#include <map>
#include <string>
#include <vector>

namespace dps::items {
    // value of one stat on an item: RoundUp(equivalency x slot share x MASTER[stat](level) x max(Quality, 0.7)
    // [x ElementalMagicMod] [x Weapon_Stats.DamageModifier]), float32 like the game. armorType or weaponType names the
    // ARMOR_EQUIVALENCY row ("" = none). stat must be the EStatType name.
    float CraftValue(const Tables& t, const std::string& stat, const std::string& slot, int level, const std::string& grade,
                     const std::string& armorType, const std::string& weaponType);
    struct PickList { std::vector<std::string> stats; int min = 0, max = 0; };
    struct Pool { std::vector<std::string> mandatory; PickList lists[3]; };   // additional, filler, mandatory-by-grade
    Pool CraftPool(const Tables& t, const std::string& tag, const std::string& grade);
    // P(stat on an item): the pick loop simulated n times (index Round(LastIndex x u), u ~ U(0,1) assumed)
    std::map<std::string, float> SpawnChance(const Tables& t, const std::string& tag, const std::string& grade, int n = 20000);
    std::string ClassArmorType(const Tables& t, const std::string& cls);              // most common armor type of the class's specs
    std::map<std::string, std::string> ClassTags(const Tables& t, const std::string& cls);   // equip slot -> most common craft tag
}

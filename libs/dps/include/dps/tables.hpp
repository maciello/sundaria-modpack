#pragma once
// DPS library (#118): the game tables the model reads, as plain data. A Source fills them; consumers never touch them.
// Names are the game's own (EStatType, BP_ItemEquipmentSlotEnum, EItemGrade, EArmorType, table row names).
#include <array>
#include <map>
#include <string>
#include <vector>

namespace dps {
    inline constexpr int kLevels = 50;        // per-level samples: index 0 = level 1 (MASTER_ITEM_STAT_TABLE)
    inline constexpr int kAbilityLevels = 5;  // ability curves: index 0 = ability level 1
    using LevelCurve = std::array<float, kAbilityLevels>;

    struct GradeRow {          // ITEMGRADE_Table row
        float quality = 1, elementalMagicMod = 1;
        int additionalMin = 0, additionalMax = 0, fillerMin = 0, fillerMax = 0, mandatoryByGradeMin = 0, mandatoryByGradeMax = 0;
    };
    struct ArmorEquivalency {  // ARMOR_EQUIVALENCY_TABLE row (row = armor type or weapon type name)
        float map = 1, rap = 1, sp = 1, baseToPlate = 1, resistElementalBaseToCloth = 1;
    };
    struct WeaponStat {        // Weapon_Stats row (row = weapon type as the spec names it: LightCrossbow, ...)
        std::string animationType;   // EWeaponType name the abilities and montages use (Crossbow, Crossbow2H, ...)
        float damageModifier = 0;
    };
    struct AttackPowerRow {    // AttackPowerTable row <Class><Melee|Ranged|Spell>
        std::string stat1, stat2;    // primary stat names (Strength, Dexterity, ...)
        float mod1 = 0, mod2 = 0, levelMod = 0;
    };
    struct CraftPool {         // CraftingBonus row (row = spec ScalableCraftingTag)
        std::vector<std::string> mandatory;                                      // Mandatory1
        std::map<std::string, std::array<std::vector<std::string>, 3>> byGrade;  // grade -> {<Grade>, Filler_<Grade>, Mandatory_Bonus_<Grade>}
    };
    struct ItemSpec {          // ItemTable_Armor / ItemTable_Weapon row
        std::string equipSlot, armorType, weaponType, tag;   // "" = none
        int randomStatType = 0, job = -1;                    // EnableRandomStats; JobClass (EClassname index, -1 = any)
    };
    struct Section { std::string name; float hits = 0, shots = 0, lockEnd = 0, length = 0; };  // lockEnd 0 = none
    struct Montage { std::string name; std::vector<Section> sections; };
    struct DamageComponent {   // one damage gameplay effect of an ability
        std::string src;
        LevelCurve coef{};               // Ability multiplier by ability level (ScalableFloat value x curve)
        int apRule = 0;                  // 0 MAP, 1 RAP, 2 SP (AttackPowerScaleRule resolved)
        bool magic = false;
        int element = 0;                 // EMagicDamageType index (magic only)
        std::string bonusStat;           // additive Ability_<X>_Damage stat, "" = none
        bool dot = false;
        LevelCurve dotDuration{}, dotPeriod{};
        int aoe = 0;                     // 0 single target, 1 every target, 2 min(targets, aoeCap)
        int aoeCap = 1;
    };
    struct Ability {
        std::string name;                          // ability folder name (RapidShots, AimedShot, ...)
        std::vector<std::string> learnedAs;        // other names the character snapshot uses for it (RapidFire, ...)
        std::vector<std::string> weaponQualifier;  // EWeaponType names; empty = any
        std::map<std::string, float> weaponPlayRate;
        LevelCurve animRate{1, 1, 1, 1, 1};
        LevelCurve cooldown{};
        std::string activateSection;
        bool projectile = false;                   // damage on projectile hit only: hits = ShootProjectile notifies
        std::vector<Montage> montages;
        std::vector<DamageComponent> damage;
    };
    struct HeroismNode { int maxPoints = 0; float perPoint = 0; };   // HeroismPassive.MaxPoints, HeroismCurve

    struct RangedAttack {      // AssignDefaultAbilityOnEquip: weapons of these types get this ability as the default attack
        std::string ability;                 // ShootArrow: only classes that own it can attack with them; "" = rule absent
        std::vector<std::string> types;      // EWeaponType names
    };

    struct Tables {
        std::string origin;                                         // where they came from (file path, "game")
        std::vector<std::string> stats;                             // EStatType names
        std::vector<std::string> percentStats;                      // DataTable_AttributeSetUI InPercentageValue
        std::map<std::string, std::array<float, kLevels>> master;   // MASTER_ITEM_STAT_TABLE by item level
        std::map<std::string, float> slotDistribution;              // SLOT_TABLE
        std::vector<std::string> ignoredDistribution;               // CDO_ItemCreation.StatTypeIgnoredDistribution
        std::map<std::string, ArmorEquivalency> armorEquivalency;
        std::vector<std::string> grades;                            // EItemGrade names by value
        std::map<std::string, GradeRow> gradeRows;
        std::vector<std::string> armorTypes, equipSlots, classes;   // EArmorType, BP_ItemEquipmentSlotEnum, EClassname
        std::map<std::string, WeaponStat> weaponStats;
        std::map<std::string, std::string> weaponDamageType;        // weapon type (spec) -> Slash | Crush | Pierce
        std::map<std::string, AttackPowerRow> attackPower;
        std::array<float, 6> primaryDefault{};                      // STR DEX INT WIS CON CHA (GlobalDefaultStats)
        int maxLevel = 20;
        std::map<std::string, CraftPool> craftPools;
        std::map<int, ItemSpec> specs;
        std::map<std::string, HeroismNode> heroism;
        RangedAttack rangedAttack;
        std::map<std::string, std::vector<Ability>> abilities;      // class -> abilities
    };
}

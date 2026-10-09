#pragma once
// DPS library (#118), public API. SDK-free; the same code runs in game (DoS-Tool.dll) and offline (libdps.so, `just dps`).
// Expected-value DPS of a build in a scenario, item scoring (DPS change in %), best in slot, stat weights.
// Thread-safety: every Model method is const and keeps no shared mutable state; call from any thread.
#include "source.hpp"
#include "tables.hpp"
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace dps {
    struct Stat { std::string name; float value = 0; };   // EStatType name as items carry it: RAP, CriticalChance, WeaponDamage_Fire

    struct Item {
        std::string name;
        int spec = 0;              // item spec id (ItemTable_Armor / ItemTable_Weapon row)
        std::string equipSlot;     // BP_ItemEquipmentSlotEnum name: Head, Body, ..., Ring, WeaponAny, WeaponLeft, WeaponDoubleHanded
        std::string weaponType;    // weapon type as the spec names it (LightCrossbow, HeavyCrossbow, ...), "" = not a weapon
        int grade = 0;             // EItemGrade value
        int level = 0;             // item level
        int slot = -1;             // equipped items: equip-container slot (tells two rings / two hands apart); -1 elsewhere
        std::vector<Stat> stats;   // rolled stats
    };

    struct Build {
        std::string cls;                                    // Champion | Cleric | Wizard | Rogue | Ranger
        int level = 1;
        std::vector<float> primary;                         // STR DEX INT WIS CON CHA; empty = game defaults
        std::vector<std::pair<std::string, int>> abilities; // learned ability -> level (char snapshot names); empty = all at abilityLevel
        int abilityLevel = 3;
        std::vector<std::pair<std::string, int>> heroism;   // heroism node -> points
        std::vector<Item> equipped;                         // both weapon sets may be present; DPS = the better set
    };

    struct Scenario {
        std::string name = "boss";
        int targets = 1;
        float fightSeconds = 120;
        int targetLevelDelta = 2;
        float armor = 400, magicResist = 400, glancing = 0, deflect = 0, incomingDamageMod = 0;
        std::vector<std::pair<std::string, float>> resist;  // damage type (Fire, Nature, Slash, ...) -> resist
        bool eventSim = false;                              // false: fluid priority rotation (smooth, default); true: discrete greedy sim
    };

    struct AbilityDps { std::string name; float dps = 0, damagePerCast = 0, castSeconds = 0, cooldown = 0, casts = 0; };
    struct Result {
        float dps = 0;
        std::string weaponType;                 // animation type of the weapon set used (Crossbow, Crossbow2H, ...)
        std::vector<AbilityDps> abilities;      // highest dps first
        std::vector<std::string> ignoredStats;  // gear stats with no reader in the damage path
        std::string error;                      // non-empty = no result (unknown class, no weapon, no damaging ability, ...)
    };
    struct ItemScore {
        float deltaPct = 0;        // DPS change in % if the item replaced `replacesSlot` (best fitting slot)
        float dps = 0;             // build DPS with the item
        int replacesSlot = -1;     // equip-container slot of the replaced item; -1 = filled an empty slot
        bool fits = false;         // the item fits a slot of this build (false: deltaPct = 0)
    };
    struct SlotPick { std::string equipSlot; int candidate = -1; };   // candidate = index into the candidates span, -1 = empty
    struct BestInSlotResult { float dps = 0; std::string weaponType; std::vector<SlotPick> picks; std::string error; };
    struct StatWeight {
        std::string stat;
        float pctBestSlot = 0;          // % DPS from one affix of this stat on the slot where it helps most
        std::string bestSlot;
        float value = 0;                // the affix value on that slot (item level = build level, given grade)
        float pctExpectedPerItem = 0;   // sum over slots of spawn chance x pct / slot count
    };

    class Prepared;   // a build + scenario compiled once for repeated scoring (opaque, immutable)

    class Model {
    public:
        static std::unique_ptr<Model> Load(Source& source, std::string& error);   // nullptr + error on failure
        explicit Model(Tables tables);
        ~Model();
        Model(const Model&) = delete;
        Model& operator=(const Model&) = delete;
        const Tables& tables() const;

        Result Dps(const Build& build, const Scenario& scenario) const;
        std::shared_ptr<const Prepared> Prepare(const Build& build, const Scenario& scenario) const;
        Result Dps(const Prepared& prepared) const;
        // slot: equip-container slot to replace; -1 = every fitting slot, best one wins. Cheap: a whole bag + bank per open.
        // An item the build already wears (same slot, spec, level, grade, stats) scores 0 with replacesSlot = its slot.
        ItemScore ScoreItem(const Prepared& prepared, const Item& item, int slot = -1) const;
        ItemScore ScoreItem(const Build& build, const Scenario& scenario, const Item& item, int slot = -1) const;
        // Best item per equip slot from candidates (gear of other heroes, bank, ...): coordinate ascent per weapon type.
        // The hero's own equipped items are ignored; its class, level, abilities, primary stats and heroism are used.
        BestInSlotResult BestInSlot(const Build& hero, const Scenario& scenario, std::span<const Item> candidates) const;
        BestInSlotResult BestInSlot(std::string_view cls, int level, const Scenario& scenario, std::span<const Item> candidates) const;
        // % DPS from one affix of each stat as it rolls on an item of each slot (item level = build level), best first.
        std::vector<StatWeight> StatWeights(const Build& build, const Scenario& scenario, int grade) const;

    private:
        struct Impl;
        std::unique_ptr<Impl> impl_;
    };
}

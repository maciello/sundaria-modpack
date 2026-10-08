#pragma once
// SDK-free item record (#16): kind, slot, stats, attack style, identity.
#include <cctype>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace items {
    enum class Where : std::uint8_t { Inventory, Bank, Equipped, Other };
    enum class Attack : std::uint8_t { Unknown, Melee, Ranged, Magic };
    enum class Kind : std::uint8_t { Other, Weapon, Armor };
    inline constexpr const char* kWhereName[] = {"Inventory", "Bank", "Equipped", "Other"};
    inline constexpr const char* kAttackName[] = {"-", "Melee", "Ranged", "Magic"};
    inline constexpr const char* kKindName[] = {"Other", "Weapon", "Armor"};

    struct Stat { int type; float value; };  // type = index into the attribute-name table (UArchonAttributeSet_Secondary floats)
    struct Item {
        Where where = Where::Other;
        bool bank = false;                // lives in the bank (storage) component
        std::uint8_t containerType = 0;   // EItemContainerType raw value
        int slot = 0, specId = 0, grade = 0, level = 0, changeId = 0;
        int equipSlot = -1;               // EBP_ItemEquipmentSlotEnum raw, -1 = not equipable
        Kind kind = Kind::Other;
        Attack attack = Attack::Unknown;
        std::string name;
        std::vector<Stat> stats;
        int weaponType = -1;              // EWeaponType raw (weapons; the animation type: a wand is Club)
        std::string typeName;             // weapon type as the spec names it (Bow, Wand, ...)
    };

    inline std::string Lower(std::string_view s) {
        std::string o(s);
        for (char& c : o) c = char(std::tolower(static_cast<unsigned char>(c)));
        return o;
    }
    inline bool Has(std::string_view hay, std::string_view needle) { return Lower(hay).find(needle) != std::string::npos; }

    // Attack of a stat name (attribute property, e.g. MeleePower_Bonus, RAP, CriticalDamage_Spell).
    // Map/RAP/SP = melee/ranged/spell attack power (unverified meaning).
    inline Attack AttackIn(std::string_view name) {
        if (name == "Map") return Attack::Melee;
        if (name == "RAP") return Attack::Ranged;
        if (name == "SP") return Attack::Magic;
        if (Has(name, "melee")) return Attack::Melee;
        if (Has(name, "range")) return Attack::Ranged;
        if (Has(name, "magic") || Has(name, "spell")) return Attack::Magic;
        return Attack::Unknown;
    }
    // Weapon: EWeaponType display name (in game: Axe, Club, Crossbow, Dagger, Fist, Sword, Shield, Axe2H, Bow2H, ...).
    inline Attack AttackOfWeapon(std::string_view weaponTypeName) {
        if (weaponTypeName.empty()) return Attack::Unknown;
        if (Has(weaponTypeName, "bow")) return Attack::Ranged;
        for (const char* m : {"staff", "wand", "orb", "scepter", "tome", "focus"})
            if (Has(weaponTypeName, m)) return Attack::Magic;
        return Attack::Melee;
    }

    // EItemContainerType display name → where. In game: bag = DefaultContainer (0), equipped = EquipContainer (1).
    inline Where WhereOf(std::string_view containerTypeName, bool bankComponent) {
        if (bankComponent) return Where::Bank;
        if (Has(containerTypeName, "equip")) return Where::Equipped;
        if (Has(containerTypeName, "default") || Has(containerTypeName, "inv") || Has(containerTypeName, "bag")) return Where::Inventory;
        return Where::Other;
    }
    inline std::string StatName(const std::vector<std::string>& names, int type) {
        return type >= 0 && type < int(names.size()) && !names[type].empty() ? names[type] : "Stat " + std::to_string(type);
    }
    // Item identity across a reorder. Not ChangedID: the game rewrites it on every reorder. Not unique (spec ids repeat).
    inline long long KeyOf(const Item& it) { return (static_cast<long long>(it.specId) << 32) | static_cast<unsigned>(it.level); }
}

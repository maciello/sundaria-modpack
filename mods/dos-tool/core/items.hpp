#pragma once
// SDK-free item model shared by item features (#16): item record, stats, kind/slot, profile weights,
// comparison buckets. Game reads: items::io (core/items.cpp, the only SDK unit for items).
#include <algorithm>
#include <cctype>
#include <cmath>
#include <compare>
#include <cstdint>
#include <map>
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

    // EItemContainerType display name → where. Unverified in game.
    inline Where WhereOf(std::string_view containerTypeName, bool bankComponent) {
        if (bankComponent) return Where::Bank;
        if (Has(containerTypeName, "equip")) return Where::Equipped;
        if (Has(containerTypeName, "main") || Has(containerTypeName, "inv") || Has(containerTypeName, "bag")) return Where::Inventory;
        return Where::Other;
    }

    // Weights 0..2 (1 = neutral). focus seeds the defaults: own-attack stats 2, other-attack stats 0.
    struct Profile {
        std::string name;
        Attack focus = Attack::Unknown;
        std::map<std::string, float> weight;  // by stat name; only values the player changed
        std::map<int, float> attack;          // weapon multiplier by Attack; only changed
    };
    inline std::vector<Profile> Presets() {
        return {{"Balanced", Attack::Unknown}, {"Melee", Attack::Melee}, {"Ranged", Attack::Ranged}, {"Magic", Attack::Magic}};
    }
    inline float DefaultWeight(Attack focus, std::string_view stat) {
        const Attack a = AttackIn(stat);
        if (focus == Attack::Unknown || a == Attack::Unknown) return 1.0f;
        return a == focus ? 2.0f : 0.0f;
    }
    inline float Weight(const Profile& p, const std::string& stat) {
        auto it = p.weight.find(stat);
        return it != p.weight.end() ? it->second : DefaultWeight(p.focus, stat);
    }
    inline std::string StatName(const std::vector<std::string>& names, int type) {
        return type >= 0 && type < int(names.size()) && !names[type].empty() ? names[type] : "Stat " + std::to_string(type);
    }

    // ---- player order (ARPG convention): groups never mix; best first inside a group ----
    // Weapons, then armor, then other equipables (jewelry), then the rest (consumables, materials).
    inline int Group(const Item& it) {
        if (it.kind == Kind::Weapon) return 0;
        if (it.kind == Kind::Armor) return 1;
        return it.equipSlot >= 0 ? 2 : 3;
    }
    // Weapon attack types in profile order: own focus first, the opposite style last.
    inline int AttackRank(Attack focus, Attack a) {
        using A = Attack;
        static constexpr A order[4][3] = {{A::Melee, A::Ranged, A::Magic},   // Balanced
                                          {A::Melee, A::Magic, A::Ranged},   // Melee
                                          {A::Ranged, A::Magic, A::Melee},   // Ranged
                                          {A::Magic, A::Ranged, A::Melee}};  // Magic
        for (int i = 0; i < 3; i++) if (order[int(focus)][i] == a) return i;
        return 3;
    }
    // Items compete on stats only inside one bucket: weapons by (attack rank, weapon type), equipables by the
    // game's equipment slot (EBP_ItemEquipmentSlotEnum order), the rest as one bucket.
    struct Bucket {
        int group, a, b;
        auto operator<=>(const Bucket&) const = default;
    };
    inline Bucket BucketOf(const Item& it, Attack focus) {
        const int g = Group(it);
        if (g == 0) return {0, AttackRank(focus, it.attack), it.weaponType};
        return {g, g == 3 ? 0 : it.equipSlot, 0};
    }
    // Weapons weigh their own attack stats (a bow's RAP, a sword's Map); everything else by the profile's focus.
    inline float WeightFor(const Profile& p, const Item& it, const std::string& stat) {
        auto w = p.weight.find(stat);
        return w != p.weight.end() ? w->second : DefaultWeight(it.kind == Kind::Weapon ? it.attack : p.focus, stat);
    }
    // Item identity across a reorder. Not ChangedID: the game rewrites it on every reorder. Not unique (spec ids repeat).
    inline long long KeyOf(const Item& it) { return (static_cast<long long>(it.specId) << 32) | static_cast<unsigned>(it.level); }
}

// Game reads (core/items.cpp). Handles are SDK pointers as void* (core/game.hpp convention).
namespace items::io {
    struct Names {
        std::vector<std::string> container, weaponType, equipSlot;  // enum value → name
        std::vector<std::string> stat;                              // Stat::type → attribute name
    };
    void Tick();                 // render thread, any rate (self-throttled to 1 Hz): enum/attribute names, bank scan
    bool Ready();                // names loaded
    const Names& GetNames();     // valid once Ready()

    // Game thread from here on (stats come from a UFunction).
    struct Located {
        void* pc = nullptr;      // ABP_PlayerControllerOnline_C
        void* inv = nullptr;     // UBP_InvManagerComponent_C
        void* bag = nullptr;     // UBP_ItemContainerComponent_C: bag (type 0) + equipped (type 1) + temp loot
        void* bank = nullptr;    // UBP_ItemContainerStorage_C
        std::string how;
    };
    Located Locate();
    std::vector<Item> Read(void* container, bool bank, bool stats);  // every container type in it
    // Bank items: live when the game has them loaded (bank open), else the last live read of this session.
    struct Bank { std::vector<Item> items; bool live = false, seen = false; };
    Bank ReadBank(bool stats);
    std::string ContainersReport();  // every live item container (class, owner, item count): debug
}

#pragma once
// SDK-free item model + profile score/order/filter (#16, #21). Shared base for Suggested (#22) and selling (#23).
#include <algorithm>
#include <cctype>
#include <cmath>
#include <compare>
#include <cstdint>
#include <map>
#include <string>
#include <string_view>
#include <vector>

namespace item_sort {
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
        int weaponType = -1;              // EWeaponType raw (weapons)
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
    // Indexes of items in player order. score[i] = Σ weight × value / (max of that stat in the item's bucket),
    // so flat stats (RAP 83) and percentages (0.03) count alike. Ties: level, grade desc; spec id, slot asc.
    // ponytail: a stat only one item in the bucket has counts in full for it; rank-based scoring if that misorders.
    inline std::vector<int> Order(const std::vector<Item>& items, const Profile& p, const std::vector<std::string>& statNames,
                                  std::vector<float>* scoreOut = nullptr) {
        std::vector<Bucket> bucket;
        std::map<std::pair<Bucket, int>, float> maxOf;
        for (const Item& it : items) {
            bucket.push_back(BucketOf(it, p.focus));
            for (const Stat& st : it.stats) {
                float& m = maxOf[{bucket.back(), st.type}];
                m = std::max(m, std::abs(st.value));
            }
        }
        std::vector<float> score(items.size());
        for (size_t i = 0; i < items.size(); i++)
            for (const Stat& st : items[i].stats)
                if (const float m = maxOf[{bucket[i], st.type}]; m > 0)
                    score[i] += WeightFor(p, items[i], StatName(statNames, st.type)) * st.value / m;
        std::vector<int> idx(items.size());
        for (int i = 0; i < int(idx.size()); i++) idx[i] = i;
        std::stable_sort(idx.begin(), idx.end(), [&](int x, int y) {
            const Item &a = items[x], &b = items[y];
            if (bucket[x] != bucket[y]) return bucket[x] < bucket[y];
            if (score[x] != score[y]) return score[x] > score[y];
            if (a.level != b.level) return a.level > b.level;
            if (a.grade != b.grade) return a.grade > b.grade;
            if (a.specId != b.specId) return a.specId < b.specId;
            return a.slot < b.slot;
        });
        if (scoreOut) *scoreOut = std::move(score);
        return idx;
    }
    struct Filter {
        bool where[4] = {true, true, true, true};
        int kind = -1;     // -1 all, else Kind
        int attack = -1;   // -1 all, else Attack (weapons)
        std::string text;  // name contains, case-insensitive
    };
    inline bool Passes(const Item& it, const Filter& f) {
        if (!f.where[int(it.where)]) return false;
        if (f.kind >= 0 && int(it.kind) != f.kind) return false;
        if (f.attack >= 0 && int(it.attack) != f.attack) return false;
        return f.text.empty() || Has(it.name, Lower(f.text));
    }

    // ---- the game's own Sort, hooked (game thread) ----
    // UFunctions that start a vanilla sort when they pass ProcessEvent. In game only the bag header's
    // BndEvt__Button_Sort_* was seen; SortItem/RequestSortItems run inside the BP VM.
    inline bool SortTrigger(std::string_view fn) {
        return fn.starts_with("BndEvt__Button_Sort") || fn == "SortItem" || fn == "RequestSortItems";
    }
    // Byte offset of the IsStorage bool in the trigger's params, -1 when it has none (the button event).
    inline int StorageParamOffset(std::string_view fn) {
        if (fn == "SortItem") return 0x0;
        if (fn == "RequestSortItems") return 0x1;
        return -1;
    }
    inline constexpr unsigned kSettleMs = 150;  // run after the vanilla sort's burst of calls

    // Positions where the container's order after applying equals the intended order (item keys).
    inline int InOrder(const std::vector<long long>& intended, const std::vector<long long>& actual) {
        int n = 0;
        for (size_t i = 0; i < intended.size() && i < actual.size(); i++) n += intended[i] == actual[i];
        return n;
    }
}

// Game-thread API for the inventory UI (implemented in item-sort.cpp; any thread may call).
namespace item_sort::api {
    std::vector<std::string> ProfileNames();
    int ActiveProfile();
    void SetActiveProfile(int i);   // persisted in dos-tool.ini
    void RequestSort(bool bank);    // runs on the game thread at the next ProcessEvent
    std::string LastStatus();       // "inventory: … applied …" or why not
}

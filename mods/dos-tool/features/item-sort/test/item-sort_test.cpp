// just test
#include "item-sort.hpp"
#include <cassert>
#include <cstdio>

using namespace item_sort;

int main() {
    const std::vector<std::string> names = {"Melee Damage", "Ranged Damage", "Armor"};
    Item bow{Where::Inventory, false, 0, 3, 100, 2, 10, 0, 1, Kind::Weapon, Attack::Ranged, "Bow", {{1, 10}, {2, 5}}};
    Item sword{Where::Bank, true, 0, 4, 200, 2, 10, 0, 1, Kind::Weapon, Attack::Melee, "Sword", {{0, 10}, {2, 5}}};
    Item helm{Where::Equipped, false, 1, 0, 300, 1, 12, 0, 2, Kind::Armor, Attack::Unknown, "Helm", {{2, 8}}};

    // player order (#21): weapons (profile attack first), armor by equip slot, jewelry, rest; groups never mix
    const std::vector<std::string> st = {"Map", "RAP", "SP", "ArmorFactor", "Ability_X"};  // 0..4
    auto W = [](int spec, Attack a, int wtype, int level, std::vector<Stat> s) {
        Item i; i.kind = Kind::Weapon; i.attack = a; i.weaponType = wtype; i.specId = spec; i.level = level; i.equipSlot = 0; i.stats = s; return i;
    };
    auto A = [](int spec, int eslot, int level, std::vector<Stat> s) {
        Item i; i.kind = Kind::Armor; i.equipSlot = eslot; i.specId = spec; i.level = level; i.stats = s; return i;
    };
    Item potion; potion.specId = 900;
    Item ring; ring.specId = 800; ring.equipSlot = 9;
    const std::vector<Item> inv = {
        potion,                                          // 0
        A(10, 3, 5, {{3, 40}, {1, 10}}),                 // 1 boots-ish slot 3
        A(11, 1, 5, {{3, 20}}),                          // 2 slot 1, weaker
        A(12, 1, 5, {{3, 30}, {4, 0.06f}}),              // 3 slot 1, better armor + ability %
        W(20, Attack::Melee, 2, 5, {{0, 120}}),          // 4 sword
        W(21, Attack::Ranged, 7, 5, {{1, 80}}),          // 5 bow
        W(22, Attack::Ranged, 7, 6, {{1, 90}}),          // 6 better bow
        W(23, Attack::Magic, 9, 5, {{2, 70}}),           // 7 staff
        ring,                                            // 8
    };
    const auto pre = Presets();
    std::vector<float> sc;
    assert((Order(inv, pre[2], st, &sc) == std::vector<int>{6, 5, 7, 4, 3, 2, 1, 8, 0}));  // Ranged
    assert((Order(inv, pre[1], st) == std::vector<int>{4, 7, 6, 5, 3, 2, 1, 8, 0}));       // Melee: reverse attack order
    // normalised inside the bucket: the bow with RAP 90 scores 1 (its max), a 0.06 ability % counts like a full stat
    assert(sc[6] == 2.0f && sc[5] < sc[6]);
    // a huge flat stat never lifts armor above weapons; groups by kind first
    Item bigArmor = A(13, 0, 1, {{3, 1e6f}});
    std::vector<Item> two = {bigArmor, W(24, Attack::Melee, 2, 1, {{0, 1}})};
    assert((Order(two, pre[0], st) == std::vector<int>{1, 0}));
    // ties: level desc, grade desc, spec asc
    Item t1 = A(30, 1, 5, {}), t2 = A(31, 1, 6, {}), t3 = A(29, 1, 6, {});
    t3.grade = 0; t2.grade = 1;
    assert((Order({t1, t2, t3}, pre[0], st) == std::vector<int>{1, 2, 0}));
    // level outranks stats inside a bucket
    assert((Order({W(40, Attack::Ranged, 7, 6, {{1, 90}, {4, 1}}), W(41, Attack::Ranged, 7, 8, {{1, 95}})}, pre[2], st) == std::vector<int>{1, 0}));
    assert(AttackOfWeapon("Wand") == Attack::Magic);
    assert(AttackRank(Attack::Ranged, Attack::Melee) == 2 && AttackRank(Attack::Melee, Attack::Ranged) == 2);
    assert(StatName(names, 9) == "Stat 9");
    Profile p{"Custom", Attack::Ranged};
    p.weight["RAP"] = 0.5f;
    assert(WeightFor(p, inv[5], "RAP") == 0.5f && WeightFor(pre[2], inv[4], "Map") == 2.0f && WeightFor(pre[2], inv[1], "Map") == 0.0f);

    // ranged/melee rule (weapon type names as logged in game)
    for (const char* m : {"Axe", "Club", "Dagger", "Fist", "Sword", "Shield", "Axe2H", "Hammer2H"}) assert(AttackOfWeapon(m) == Attack::Melee);
    assert(AttackOfWeapon("Crossbow") == Attack::Ranged && AttackOfWeapon("Bow2H") == Attack::Ranged && AttackOfWeapon("Crossbow2H") == Attack::Ranged);
    assert(AttackOfWeapon("Staff2H") == Attack::Magic && AttackOfWeapon("") == Attack::Unknown);
    assert(AttackIn("MeleePower_Bonus") == Attack::Melee && AttackIn("RAP") == Attack::Ranged && AttackIn("CriticalDamage_Spell") == Attack::Magic);
    assert(AttackIn("Damage_Slash") == Attack::Unknown);
    assert(WhereOf("Equipment", false) == Where::Equipped);
    assert(WhereOf("MainInventory", false) == Where::Inventory);
    assert(WhereOf("Equipment", true) == Where::Bank);

    // filter
    Filter f;
    assert(Passes(bow, f));
    f.where[int(Where::Bank)] = false;
    assert(!Passes(sword, f));
    f.kind = int(Kind::Weapon);
    assert(!Passes(helm, f) && Passes(bow, f));
    f.text = "BO";
    assert(Passes(bow, f));
    f.attack = int(Attack::Melee);
    assert(!Passes(bow, f));

    // vanilla sort hook
    assert(SortTrigger("BndEvt__Button_Sort_K2Node_ComponentBoundEvent_974_OnButtonClickedEvent__DelegateSignature"));
    assert(SortTrigger("SortItem") && SortTrigger("RequestSortItems"));
    assert(!SortTrigger("SortItemsInternalClient") && !SortTrigger("ReorderItems") && !SortTrigger("Tick"));
    assert(StorageParamOffset("RequestSortItems") == 1 && StorageParamOffset("BndEvt__Button_Sort_x") == -1);
    assert(InOrder({1, 2, 3}, {1, 3, 2}) == 1 && InOrder({1, 2}, {1, 2}) == 2);
    std::puts("ok");
}

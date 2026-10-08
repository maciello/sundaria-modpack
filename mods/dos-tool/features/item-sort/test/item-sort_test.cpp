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

    // score = weighted sum (#16)
    Profile flat{"Balanced"};
    assert(Score(helm, flat, names) == 8);
    assert(Score(bow, flat, names) == 15);
    Profile p{"Custom"};
    p.weight["Armor"] = 0.5f;
    assert(Score(helm, p, names) == 4);
    assert(StatName(names, 9) == "Stat 9");

    // ranged profile puts the bow first, melee the sword; switching changes the order (#21)
    const std::vector<Item> items = {sword, helm, bow};
    auto first = [&](const Profile& pr) {
        std::vector<float> s;
        for (const Item& it : items) s.push_back(Score(it, pr, names));
        return Reorder({10, 11, 12}, {0, 1, 2}, items, s).front();
    };
    const auto pre = Presets();
    assert(first(pre[2]) == 12);  // Ranged: bow (10*2+5)*2 = 50
    assert(first(pre[1]) == 10);  // Melee: sword
    assert(Score(bow, pre[1], names) == (0 * 10 + 5) * 0.5f);

    // ties: level desc, then spec id asc
    Item a = helm, b = helm;
    b.level = 13;
    assert(Before(b, 1, a, 1));
    b = helm; b.specId = 299;
    assert(Before(b, 1, a, 1) && !Before(a, 1, b, 1));

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

    // game reorder: plain slots recognised, compressed not; permutation best-first, refuses unknown/duplicate
    std::vector<Item> inv = {bow, helm, sword};
    inv[2].bank = false; inv[2].slot = 7;
    assert(PlainType({7, 3}, inv) == 0);
    assert(PlainType({0}, inv) == 1);
    assert(PlainType({103, 107}, inv) == -1);
    std::vector<float> s = {50, 1, 60};
    assert((Reorder({103, 107}, {0, 2}, inv, s) == std::vector<int>{107, 103}));
    assert(Reorder({103, 107}, {0, 0}, inv, s).empty());
    assert(Reorder({103, 107}, {0, -1}, inv, s).empty());
    assert(Reorder({}, {}, inv, s).empty());
    Filter armorFirst;
    armorFirst.kind = int(Kind::Armor);
    assert((Reorder({103, 100, 107}, {0, 1, 2}, inv, s, armorFirst) == std::vector<int>{100, 107, 103}));

    // vanilla sort hook
    assert(SortTrigger("BndEvt__Button_Sort_K2Node_ComponentBoundEvent_974_OnButtonClickedEvent__DelegateSignature"));
    assert(SortTrigger("SortItem") && SortTrigger("RequestSortItems"));
    assert(!SortTrigger("SortItemsInternalClient") && !SortTrigger("ReorderItems") && !SortTrigger("Tick"));
    assert(StorageParamOffset("RequestSortItems") == 1 && StorageParamOffset("BndEvt__Button_Sort_x") == -1);
    assert(InOrder({1, 2, 3}, {1, 3, 2}) == 1 && InOrder({1, 2}, {1, 2}) == 2);
    std::puts("ok");
}

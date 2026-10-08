// just test
#include "item-sort.hpp"
#include <cassert>
#include <cstdio>

using namespace item_sort;

int main() {
    const std::vector<std::string> names = {"Melee Damage", "Ranged Damage", "Armor"};
    Item bow{Where::Inventory, false, 0, 3, 100, 2, 10, 1, Kind::Weapon, Attack::Ranged, "Bow", {{1, 10}, {2, 5}}};
    Item sword{Where::Bank, true, 0, 4, 200, 2, 10, 1, Kind::Weapon, Attack::Melee, "Sword", {{0, 10}, {2, 5}}};
    Item helm{Where::Equipped, false, 1, 0, 300, 1, 12, 2, Kind::Armor, Attack::Unknown, "Helm", {{2, 8}}};

    // score = weighted sum (#16)
    Profile flat{"Balanced"};
    assert(Score(helm, flat, names) == 8);
    assert(Score(bow, flat, names) == 15);
    Profile p{"Custom"};
    p.weight["Armor"] = 0.5f;
    assert(Score(helm, p, names) == 4);
    assert(StatName(names, 9) == "Stat 9");

    // ranged profile puts ranged first, melee profile melee first; switching changes the order (#21)
    const std::vector<Item> items = {sword, helm, bow};
    auto order = [&](const Profile& pr) {
        std::vector<float> s;
        for (const Item& it : items) s.push_back(Score(it, pr, names));
        return Order(items, s);
    };
    const auto pre = Presets();
    assert(order(pre[2]).front() == 2);  // Ranged: bow (10*2+5)*2 = 50
    assert(order(pre[1]).front() == 0);  // Melee: sword
    assert(Score(bow, pre[1], names) == (0 * 10 + 5) * 0.5f);

    // ties: level desc, then spec id asc
    Item a = helm, b = helm;
    b.level = 13;
    assert(Before(b, 1, a, 1));
    b = helm; b.specId = 299;
    assert(Before(b, 1, a, 1) && !Before(a, 1, b, 1));

    // ranged/melee rule
    assert(AttackOf("NewEnumerator0", "Longbow") == Attack::Ranged);
    assert(AttackOf("Melee", "Bow") == Attack::Melee);
    assert(AttackOf("Magic", "") == Attack::Magic);
    assert(AttackOf("x", "y") == Attack::Unknown);
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
    std::puts("ok");
}

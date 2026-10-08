// just test
#include "item-sell.hpp"
#include <cassert>
#include <cstdio>

using namespace item_sell;

int main() {
    const std::vector<std::string> st = {"Map", "RAP", "SP", "ArmorFactor", "Ability_X"};  // 0..4
    auto W = [](const char* type, int level, std::vector<Stat> s, Where w = Where::Inventory) {
        Item i; i.kind = Kind::Weapon; i.typeName = type; i.attack = AttackOfWeapon(type); i.level = level; i.equipSlot = 0; i.stats = s; i.where = w;
        return i;
    };
    auto A = [](int eslot, int level, std::vector<Stat> s, Where w = Where::Inventory) {
        Item i; i.kind = Kind::Armor; i.equipSlot = eslot; i.level = level; i.stats = s; i.where = w; return i;
    };
    const auto pre = Presets();
    const Profile& ranged = pre[2];

    // dominance: same slot, not lower level/grade, every valued stat at least as high
    assert(AtLeast(W("Bow", 8, {{1, 90}}), W("Bow", 6, {{1, 80}}), ranged, st));
    assert(!AtLeast(W("Bow", 8, {{1, 70}}), W("Bow", 6, {{1, 80}}), ranged, st));          // lower RAP
    assert(!AtLeast(W("Bow", 5, {{1, 99}}), W("Bow", 6, {{1, 80}}), ranged, st));          // lower level
    assert(!AtLeast(W("Crossbow", 8, {{1, 90}}), W("Bow", 6, {{1, 80}}), ranged, st));     // other weapon type
    assert(!AtLeast(W("Bow", 8, {{1, 90}}), W("Bow", 6, {{1, 80}, {4, 0.03f}}), ranged, st));  // b has a stat a lacks
    assert(AtLeast(A(1, 5, {{3, 30}}), A(1, 5, {{3, 30}, {0, 50}}), ranged, st));          // Map is worth 0 to a Ranged profile on armor
    assert(!AtLeast(A(1, 5, {{3, 30}}), A(1, 5, {{3, 30}, {0, 50}}), pre[1], st));         // ... but not to Melee

    // suggestions: worse bow in the bank (dominated by an equipped bow) sold; the best of a slot and equipped items kept;
    // a lone crossbow kept; an un-dominated second bow kept; potions never
    Item potion;
    const std::vector<Item> owned = {
        W("Bow", 8, {{1, 90}}, Where::Equipped),   // 0 best, equipped
        W("Bow", 6, {{1, 80}}, Where::Bank),       // 1 dominated by 0
        W("Bow", 7, {{1, 60}, {4, 0.05f}}),        // 2 has an ability stat 0 lacks: kept
        W("Crossbow", 4, {{1, 40}}),               // 3 only one of its type
        A(1, 5, {{3, 30}}),                        // 4 best helm
        A(1, 5, {{3, 30}}),                        // 5 identical helm: the copy goes
        potion, potion,                            // 6, 7
    };
    const std::vector<Pick> s = Suggest(owned, ranged, st);
    assert(s.size() == 2);
    assert(s[0].item == 1 && s[0].by == 0);
    assert(s[1].item == 5 && s[1].by == 4);
    // keep 3 per slot: all three bows stay (the bank bow ranks 3rd: lv 8, 7, 6)
    for (const Pick& p : Suggest(owned, ranged, st, 3)) assert(p.item != 1);
    // an equipped item is never suggested even when something beats it
    std::vector<Item> eq = {W("Bow", 9, {{1, 99}}), W("Bow", 8, {{1, 90}}, Where::Equipped)};
    assert(Suggest(eq, ranged, st).empty());
    std::puts("ok");
}

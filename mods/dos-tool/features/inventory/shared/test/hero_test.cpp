// just test
#include "hero.hpp"
#include <cassert>
#include <cstdio>

using namespace items::hero;

int main() {
    assert(Quote("a\"b\\c\nd") == "\"a\\\"b\\\\c\\nd\"");
    assert(Flow(std::vector<int>{}) == "[]" && Flow(std::vector<float>{18, 0.5f}) == "[18, 0.5]");

    Snapshot s;
    s.slot = 2;
    s.name = "Hero";
    s.cls = "Ranger";
    s.level = 10;
    s.learned = {{"RapidShot", 3}};
    s.bar = {{311, "Ability", false, {6, 30}}};
    items::Item bow;
    bow.slot = 4;
    bow.equipSlot = 1;
    bow.name = "Bow";
    bow.specId = 7;
    bow.kind = items::Kind::Weapon;
    bow.typeName = "Bow2H";
    bow.level = 9;
    bow.stats = {{0, 12.5f}, {5, 1}};
    s.equipped = {bow};
    const std::string y = Yaml(s, {"RAP"}, {"Head", "MainHand"});
    const std::string want =
        "# dos-tool character snapshot (#102): written when the game saves or loads this hero\n"
        "slot: 2\nname: \"Hero\"\nclass: \"Ranger\"\nlevel: 10\nheroism_level: 0\nheroism_points: []\nprimary_stats: []\n"
        "learned_abilities:\n  - {name: \"RapidShot\", level: 3}\n"
        "action_bar:\n  - {id: 311, type: \"Ability\", passive: false, slots: [6, 30]}\n"
        "equipped:\n  - slot: 4\n    equip_slot: \"MainHand\"\n    name: \"Bow\"\n    spec: 7\n    kind: Weapon\n    type: \"Bow2H\"\n"
        "    grade: 0\n    level: 9\n    stats:\n      RAP: 12.5\n      Stat 5: 1\n";
    if (y != want) std::puts(y.c_str());
    assert(y == want);
    assert(Yaml(Snapshot{}, {}, {}).find("equipped: []\n") != std::string::npos);

    // read back what Yaml wrote
    s.name = "He said \"hi\"";
    s.primaryStats = {18, 20.5f};
    s.learned.push_back({"AimedShot", 2});
    items::Item ring;
    ring.slot = 13;
    ring.equipSlot = 0;
    ring.name = "Ring";
    ring.kind = items::Kind::Armor;
    ring.grade = 4;
    s.equipped.push_back(ring);
    const Read r = Parse(Yaml(s, {"RAP"}, {"Head", "MainHand"}));
    assert(r.slot == 2 && r.name == "He said \"hi\"" && r.cls == "Ranger" && r.level == 10);
    assert(r.primaryStats.size() == 2 && r.primaryStats[1] == 20.5f);
    assert(r.learned.size() == 2 && r.learned[1].name == "AimedShot" && r.learned[1].level == 2);
    assert(r.equipped.size() == 2);
    const Gear& g = r.equipped[0];
    assert(g.slot == 4 && g.equipSlot == "MainHand" && g.name == "Bow" && g.spec == 7 && g.kind == "Weapon" && g.type == "Bow2H" && g.level == 9);
    assert(g.stats.size() == 2 && g.stats[0].first == "RAP" && g.stats[0].second == 12.5f && g.stats[1].first == "Stat 5");
    assert(r.equipped[1].equipSlot == "Head" && r.equipped[1].grade == 4 && r.equipped[1].stats.empty() && r.equipped[1].type.empty());
    assert(Parse("garbage\n").slot == -1);
    std::puts("ok");
}

// just test
#include "char-snapshot.hpp"
#include <cassert>
#include <cstdio>

using namespace char_snapshot;

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
    std::puts("ok");
}

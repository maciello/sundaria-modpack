// just test
#include "ability-dump.hpp"
#include <cassert>
#include <cstdio>

using namespace ability_dump;

int main() {
    NotifyCounts n;
    for (int t : {1, 1, 1, 0, 2, 3, 9}) n.Add(t);
    assert(n.Hits() == 4 && n.other == 1 && n.byType[AnimLockEnd] == 1);

    Ability a;
    a.cls = "UBP_GameAbility_RapidShot_C";
    a.tags = {"Ability.RapidShot", "A\"b"};
    a.cooldown = {"UGE_RapidShot_CoolDown_C", 0, 12.5f, ""};
    a.montages.push_back({"/Game/M/Rapid", true, 2.0f, n});
    a.montages.push_back({"/Game/M/Unloaded", false, 0, {}});
    Live live;
    live.valid = true; live.ability = a.cls; live.montage = a.montages[0];
    const std::string y = ToYaml({a}, live);
    std::puts(y.c_str());
    assert(y.find("ShootProjectile: 3") != std::string::npos);
    assert(y.find("hits: 4") != std::string::npos);
    assert(y.find("value: 12.5") != std::string::npos);
    assert(y.find("\"A\\\"b\"") != std::string::npos);
    assert(y.find("castTime: null") != std::string::npos);
    assert(y.find("loaded: false\n      - ") == std::string::npos && y.find("loaded: false\n    ") == std::string::npos);
    assert(ToYaml({}, {}).find("playing: null") != std::string::npos);
    std::puts("ok");
}

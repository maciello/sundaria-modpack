// just test
#include "item-upgrade.hpp"
#include <cassert>
#include <cstdio>

using namespace item_upgrade;

int main() {
    assert(Pct(4.24f) == "+4.2%" && Pct(1.0f) == "+1.0%" && Pct(12.4f) == "+12%" && Pct(9.96f) == "+10%");
    assert(Cut("dorfmatratze", 8) == "dorfmatr" && Cut("Ana", 8) == "Ana" && Cut("\xC3\xA4\xC3\xB6x", 2) == "\xC3\xA4\xC3\xB6");

    // upgrade for the current hero wins the chip; the best other hero still gets its line
    const std::vector<Other> others = {{"Aria", "Wizard", 3.0f, true}, {"Brom", "Champion", 6.1f, true}, {"Cid", "Rogue", 50, false}};
    Verdict v = Judge(4.2f, true, others, {"Ranger"});
    assert(v.mark == Mark::Upgrade && Chip(v) == "+4.2%");
    assert(Lines(v) == "Upgrade: +4.2% DPS\nBetter for Brom (Champion): +6.1% DPS\nBest in slot: Ranger");

    // below the threshold or not fitting: the other hero's mark
    v = Judge(0.9f, true, others, {});
    assert(v.mark == Mark::OtherHero && Chip(v) == "Brom" && Lines(v) == "Better for Brom (Champion): +6.1% DPS");
    assert(Judge(30, false, others, {}).mark == Mark::OtherHero);

    // nothing worth saying
    v = Judge(0.5f, true, {{"Aria", "Wizard", 0.99f, true}}, {});
    assert(v.mark == Mark::None && Chip(v).empty() && Lines(v).empty());
    assert(Lines(Judge(0, false, {}, {"Ranger", "Rogue"})) == "Best in slot: Ranger, Rogue");

    // cache key: same item = same key; any stat, level or grade change = new key
    struct S { std::string name; float value; };
    const std::vector<S> a = {{"RAP", 17}, {"Health", 12}};
    assert(Key(1, 8, 4, a) == Key(1, 8, 4, a));
    assert(Key(1, 8, 4, a) != Key(1, 9, 4, a) && Key(1, 8, 4, a) != Key(1, 8, 5, a));
    assert(Key(1, 8, 4, a) != Key(1, 8, 4, std::vector<S>{{"RAP", 18}, {"Health", 12}}));
    std::puts("ok");
}

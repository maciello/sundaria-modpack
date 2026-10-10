// just test: the DPS library on synthetic tables (made-up numbers, no game data).
#include "../src/items.hpp"
#include "dps/dps.hpp"
#include <cassert>
#include <cmath>
#include <cstdio>
#include <fstream>

using namespace dps;

static bool Near(float a, float b, float tol = 1e-3f) { return std::fabs(a - b) <= tol * std::max(1.f, std::fabs(b)); }

// one class (Ranger), one 2H bow ability: 2 hits per 1 s cast, no cooldown, coefficient 1, RAP scaling, physical
static Tables Synthetic() {
    Tables t;
    t.classes = {"Champion", "Cleric", "Wizard", "Rogue", "Ranger"};
    t.stats = {"Health", "MAP", "RAP", "SP", "CriticalChance", "CriticalDamage_Melee", "WeaponDamage"};
    t.percentStats = {"CriticalChance", "CriticalDamage_Melee"};
    t.master["RAP"].fill(100.f), t.master["Health"].fill(50.f), t.master["CriticalChance"].fill(0.1f);
    t.master["CriticalDamage_Melee"].fill(0.05f), t.master["WeaponDamage"].fill(40.f);
    t.slotDistribution = {{"Ring", 0.5f}, {"WeaponLeft", 1.f}, {"WeaponDoubleHanded", 1.f}, {"Neck", 0.1f}};
    t.grades = {"Poor", "Common", "Superior", "Rare"};
    t.gradeRows["Rare"] = {1.f, 1.f, 1, 1, 0, 0, 0, 0};
    t.gradeRows["Common"] = {2.f, 1.f, 0, 0, 0, 0, 0, 0};
    t.weaponStats["Longbow"] = {"Bow2H", 2.5f};
    t.weaponStats["Greatbow"] = {"Bow2H", 2.5f};
    t.weaponStats["Arbalest"] = {"Crossbow2H", 2.5f};
    t.weaponStats["Shortsword"] = {"Sword", 2.5f};
    t.weaponDamageType = {{"Shortsword", "Slash"}, {"Longbow", "Pierce"}, {"Greatbow", "Pierce"}, {"Arbalest", "Pierce"}};
    t.attackPower["RangerRanged"] = {"Dexterity", "Intelligence", 2.f, 1.f, 1.f};
    t.primaryDefault.fill(10.f);
    t.maxLevel = 20;
    t.craftPools["RingTag"].mandatory = {"RAP"};
    t.craftPools["RingTag"].byGrade["Rare"][0] = {"RAP", "Health", "CriticalChance"};
    t.craftPools["BowTag"].mandatory = {"WeaponDamage"};
    t.specs[1] = {"Ring", "", "", "RingTag", 8, 4};
    t.specs[2] = {"WeaponDoubleHanded", "", "Longbow", "BowTag", 8, 4};
    Ability a;
    a.name = "Shot", a.weaponQualifier = {"Bow2H"}, a.activateSection = "Fire";
    a.montages = {{"Mon_Shot_H_M_Bow2H", {{"Fire", 2, 0, 0, 1.f}}}, {"Mon_Shot_H_M_Crossbow2H", {{"Fire", 9, 0, 0, 1.f}}}};
    DamageComponent d;
    d.src = "GE_Shot", d.coef.fill(1.f), d.apRule = 1;
    a.damage = {d};
    t.abilities["Ranger"] = {a};
    t.rangedAttack = {"Shot", {"Bow2H", "Crossbow2H"}};
    Ability stab;   // a Rogue: melee ability, any weapon, no ranged default attack
    stab.name = "Stab", stab.activateSection = "Hit";
    stab.montages = {{"Mon_Stab_H_M", {{"Hit", 1, 0, 0, 1.f}}}};
    stab.damage = {d};
    t.abilities["Rogue"] = {stab};
    t.attackPower["RogueMelee"] = {"Strength", "Dexterity", 1.f, 1.f, 1.f};
    return t;
}

static Item It(std::string name, std::string slot, std::vector<Stat> stats, std::string weapon = "", int equipSlot = -1) {
    Item i;
    i.name = std::move(name), i.equipSlot = std::move(slot), i.weaponType = std::move(weapon), i.slot = equipSlot, i.stats = std::move(stats);
    return i;
}

int main() {
    Model m{Synthetic()};
    Scenario sc;
    sc.armor = sc.magicResist = 0, sc.targetLevelDelta = 0, sc.fightSeconds = 10;
    Build b;
    b.cls = "Ranger", b.level = 10;
    b.equipped = {It("bow", "WeaponDoubleHanded", {{"WeaponDamage", 100}, {"RAP", 180}, {"Health", 9}}, "Longbow", 8)};

    // AP = 2x10 + 1x10 + (10/20 + 1) x 10 x 1 = 40, + 180 gear = 220 -> AP term 1. Hit = 0.95 + 0.05 x 1.5.
    // per cast 2 x 100 x 1.025 = 205, one cast per second
    const Result r = m.Dps(b, sc);
    assert(r.error.empty() && r.weaponType == "Bow2H");
    assert(Near(r.dps, 205.f) && r.abilities.size() == 1 && Near(r.abilities[0].casts, 10.f));
    assert(r.ignoredStats == std::vector<std::string>{"Health"});

    // scoring: a ring doubling AP fills an empty ring slot (+100 %); trash fits with 0; replacing the bow
    auto p = m.Prepare(b, sc);
    const ItemScore ring = m.ScoreItem(*p, It("ring", "Ring", {{"RAP", 220}}));
    assert(ring.fits && ring.replacesSlot == -1 && Near(ring.deltaPct, 100.f));
    const ItemScore trash = m.ScoreItem(*p, It("hp ring", "Ring", {{"Health", 50}}));
    assert(trash.fits && Near(trash.deltaPct, 0.f));
    const ItemScore bow = m.ScoreItem(*p, It("greatbow", "WeaponDoubleHanded", {{"WeaponDamage", 150}, {"RAP", 180}}, "Greatbow"));
    assert(bow.fits && bow.replacesSlot == 8 && Near(bow.deltaPct, 50.f));
    // a weapon type no ability uses: the set compiles nothing, DPS 0
    const ItemScore xbow = m.ScoreItem(*p, It("arbalest", "WeaponDoubleHanded", {{"WeaponDamage", 999}}, "Arbalest"));
    assert(xbow.fits && Near(xbow.deltaPct, -100.f));
    assert(!m.ScoreItem(*p, It("potion", "", {{"RAP", 999}})).fits);
    // 2 rings fit: the second ring fills the other slot, then replaces the weaker one
    b.equipped.push_back(It("r1", "Ring", {{"RAP", 10}}, "", 14));
    b.equipped.push_back(It("r2", "Ring", {{"RAP", 30}}, "", 15));
    auto p2 = m.Prepare(b, sc);
    const ItemScore up = m.ScoreItem(*p2, It("r3", "Ring", {{"RAP", 20}}));
    assert(up.fits && up.replacesSlot == 14 && up.deltaPct > 0.f);
    assert(m.ScoreItem(*p2, It("r3", "Ring", {{"RAP", 20}}), 15).deltaPct < 0.f);   // forced into slot 15
    const ItemScore worn = m.ScoreItem(*p2, b.equipped[2]);
    assert(worn.fits && worn.replacesSlot == 15 && worn.deltaPct == 0.f);

    // active weapon set (slots 7+8 / 17+18 / 19+20): only it counts; -1 = the better set
    {
        Build two = b;
        two.equipped = {It("bow", "WeaponDoubleHanded", {{"WeaponDamage", 100}, {"RAP", 180}}, "Longbow", 8),
                        It("greatbow", "WeaponDoubleHanded", {{"WeaponDamage", 150}, {"RAP", 180}}, "Greatbow", 18)};
        const Item mid = It("mid", "WeaponDoubleHanded", {{"WeaponDamage", 120}, {"RAP", 180}}, "Longbow");
        auto score = [&](int set) { two.activeSet = set; return m.ScoreItem(*m.Prepare(two, sc), mid); };
        assert(Near(score(0).deltaPct, 20.f) && score(0).replacesSlot == 8);       // set 0 holds the 100 bow
        assert(score(1).deltaPct < 0.f && score(1).replacesSlot == 18);            // set 1 holds the 150 bow
        two.activeSet = 2;
        assert(!score(2).fits);   // an empty set: nothing to compare
    }

    // the slot the game compares: first ring / trinket / hand while free, ALT = the second
    {
        Build t;
        t.cls = "Rogue", t.level = 10;
        const Item ring = It("r", "Ring", {{"RAP", 1}}), sword = It("s", "WeaponAny", {}, "Shortsword"), bow = It("b", "WeaponDoubleHanded", {}, "Longbow"),
                   xbow = It("x", "WeaponDoubleHanded", {}, "Arbalest");
        assert(m.TargetSlot(t, ring, false) == 14 && m.TargetSlot(t, ring, true) == 14);
        t.equipped = {It("r1", "Ring", {}, "", 14)};
        assert(m.TargetSlot(t, ring, false) == 15 && m.TargetSlot(t, ring, true) == 15);
        t.equipped.push_back(It("r2", "Ring", {}, "", 15));
        assert(m.TargetSlot(t, ring, false) == 14 && m.TargetSlot(t, ring, true) == 15);
        assert(m.TargetSlot(t, It("t", "Trinket", {}), true) == 9 && m.TargetSlot(t, It("h", "Head", {}), false) == -1);
        assert(m.TargetSlot(t, sword, false) == 8 && m.TargetSlot(t, bow, false) == 7 && m.TargetSlot(t, xbow, false) == 8);   // bows go left
        t.equipped = {It("s1", "WeaponAny", {}, "Shortsword", 8)};
        assert(m.TargetSlot(t, sword, false) == 7);                                // right hand taken, left free
        t.equipped.push_back(It("s2", "WeaponAny", {}, "Shortsword", 7));
        assert(m.TargetSlot(t, sword, false) == 8 && m.TargetSlot(t, sword, true) == 7);   // both taken: ALT = the left hand
        t.activeSet = 1;
        assert(m.TargetSlot(t, sword, false) == 18 && m.TargetSlot(t, bow, false) == 17);   // set 1 is empty
        // scoring into a given slot: an empty ring slot is filled, a worn one replaced
        assert(m.ScoreItem(*p, ring, 14).replacesSlot == -1 && m.ScoreItem(*p, ring, 14).fits);
        assert(m.ScoreItem(*p2, It("r3", "Ring", {{"RAP", 20}}), 14).replacesSlot == 14);
    }

    // best in slot: the stronger bow and the two RAP rings, the Health ring stays out
    std::vector<Item> cands = {It("bow", "WeaponDoubleHanded", {{"WeaponDamage", 100}}, "Longbow"),
                               It("greatbow", "WeaponDoubleHanded", {{"WeaponDamage", 150}}, "Greatbow"),
                               It("rap", "Ring", {{"RAP", 220}}), It("hp", "Ring", {{"Health", 50}}), It("rap small", "Ring", {{"RAP", 10}})};
    const BestInSlotResult bis = m.BestInSlot("Ranger", 10, sc, cands);
    assert(bis.error.empty() && bis.weaponType == "Bow2H");
    int picked[5] = {};
    for (const SlotPick& s : bis.picks) if (s.candidate >= 0) picked[s.candidate]++;
    assert(!picked[0] && picked[1] && picked[2] && !picked[3] && picked[4]);

    // use rules: item level above the hero's, and ranged weapons for a class without the ranged default attack
    {
        Item lvl = It("greatbow", "WeaponDoubleHanded", {{"WeaponDamage", 150}, {"RAP", 180}}, "Greatbow");
        lvl.level = 11;
        assert(!m.ScoreItem(*p, lvl).fits);
        lvl.level = 10;
        assert(m.ScoreItem(*p, lvl).fits);
        std::vector<Item> over = {lvl, It("bow", "WeaponDoubleHanded", {{"WeaponDamage", 100}}, "Longbow")};
        over[0].level = 11, over[0].stats = {{"WeaponDamage", 999}};
        assert(m.BestInSlot("Ranger", 10, sc, over).picks[0].candidate == 1);   // the level-11 bow is skipped
        Build rogue;
        rogue.cls = "Rogue", rogue.level = 1;
        rogue.equipped = {It("sword", "WeaponAny", {{"WeaponDamage", 10}}, "Shortsword", 7)};
        auto rp = m.Prepare(rogue, sc);
        assert(m.Dps(*rp).error.empty());
        assert(!m.ScoreItem(*rp, It("arbalest", "WeaponDoubleHanded", {{"WeaponDamage", 999}}, "Arbalest")).fits);
        assert(!m.ScoreItem(*rp, It("longbow", "WeaponDoubleHanded", {{"WeaponDamage", 999}}, "Longbow")).fits);
    }

    // item generation: value = equivalency x slot share x MASTER x quality, rounded up (0.01 for percent stats)
    const Tables& t = m.tables();
    assert(items::CraftValue(t, "RAP", "Ring", 10, "Rare", "", "") == 50.f);
    assert(items::CraftValue(t, "WeaponDamage", "WeaponDoubleHanded", 10, "Rare", "", "Longbow") == 100.f);
    // float32 like the game: 0.05 x 0.1 x 2.0 lands just above 0.01 and rounds up to 0.02
    assert(items::CraftValue(t, "CriticalDamage_Melee", "Neck", 10, "Common", "", "") == 0.02f);
    const auto spawn = items::SpawnChance(t, "RingTag", "Rare", 4000);
    assert(spawn.at("RAP") == 1.f && Near(spawn.at("Health"), 0.5f, 0.1f) && !items::CraftPool(t, "RingTag", "Rare").lists[0].stats.empty());
    assert(items::CraftPool(t, "RingTag", "Rare").lists[0].stats.size() == 2);   // mandatory RAP removed from the list

    // stat weights: +100 weapon damage on the bow (+100 %) beats +50 RAP on a ring; Health has no reader
    const auto w = m.StatWeights(b, sc, 3);
    assert(w.size() == 4 && w[0].stat == "WeaponDamage" && Near(w[0].pctBestSlot, 100.f) && w[0].bestSlot == "WeaponDoubleHanded");
    assert(w[1].stat == "RAP" && w[1].bestSlot == "Ring" && w[1].value == 50.f && Near(w[1].pctBestSlot, 50.f / 260.f * 100.f));
    for (const auto& x : w) if (x.stat == "Health") assert(x.pctBestSlot == 0.f);

    // offline source: one file in, same tables out; a bad record names its line
    {
        std::ofstream f("build/dps_test_tables.txt");
        f << "dps-tables 1\nstats RAP\nmaster RAP";
        for (int i = 0; i < kLevels; ++i) f << " 1";
        f << "\nweapon Longbow Bow2H 2.5\nability Ranger Shot 0 Fire\nqualifier Bow2H\nmontage Mon_Shot\nsection Fire 2 0 0 1\n"
             "damage GE_Shot 1 0 0 - 0 0 1\ncoef 1 1 1 1 1\n";
    }
    std::string err;
    auto src = FileSource("build/dps_test_tables.txt");
    auto loaded = Model::Load(*src, err);
    assert(loaded && err.empty() && loaded->tables().abilities.at("Ranger")[0].montages[0].sections[0].hits == 2.f);
    { std::ofstream f("build/dps_test_tables.txt", std::ios::app); f << "coef 1 x\n"; }
    assert(!Model::Load(*FileSource("build/dps_test_tables.txt"), err) && err.find(":11:") != std::string::npos);
    std::puts("ok");
}

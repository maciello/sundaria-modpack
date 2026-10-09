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
    t.weaponDamageType = {{"Longbow", "Pierce"}, {"Greatbow", "Pierce"}, {"Arbalest", "Pierce"}};
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

    // best in slot: the stronger bow and the two RAP rings, the Health ring stays out
    std::vector<Item> cands = {It("bow", "WeaponDoubleHanded", {{"WeaponDamage", 100}}, "Longbow"),
                               It("greatbow", "WeaponDoubleHanded", {{"WeaponDamage", 150}}, "Greatbow"),
                               It("rap", "Ring", {{"RAP", 220}}), It("hp", "Ring", {{"Health", 50}}), It("rap small", "Ring", {{"RAP", 10}})};
    const BestInSlotResult bis = m.BestInSlot("Ranger", 10, sc, cands);
    assert(bis.error.empty() && bis.weaponType == "Bow2H");
    int picked[5] = {};
    for (const SlotPick& s : bis.picks) if (s.candidate >= 0) picked[s.candidate]++;
    assert(!picked[0] && picked[1] && picked[2] && !picked[3] && picked[4]);

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

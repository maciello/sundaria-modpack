// just test
#include "idle-loot.hpp"
#include <cassert>
#include <cmath>
#include <cstdio>
#include <string>

int main() {
    using namespace idle_loot;
    using loot::Actor;
    using loot::Kind;
    // a pile takes its best grade; far loot keeps its own; looted loot gets none and lends none
    const std::vector<Actor> a = {
        {1, Kind::Item, 0, 0, 0, true, 1, NAN},
        {2, Kind::Item, 100, 0, 0, true, 4, NAN},       // 1 m away: same pile, Epic
        {3, Kind::Item, 1000, 0, 0, true, 2, NAN},      // 10 m away: own pile
        {4, Kind::Item, 50, 0, 0, false, 7, NAN},       // taken: no sparkle, no colour lent
        {5, Kind::Chest, 1000, 100, 0, true, -1, NAN},  // closed chest, contents unknown, next to item 3
    };
    const std::vector<int> g = PileGrades(a);
    assert(g[0] == 4 && g[1] == 4);
    assert(g[2] == 2 && g[4] == 2);
    assert(g[3] == kLooted);
    assert(PileGrades({{9, Kind::Chest, 0, 0, 0, true, -1, NAN}})[0] == -1);

    // colour: sRGB round trip, purple stays purple, unknown grade falls back to soft text
    for (int s = 0; s < 256; s++) assert(loot::ToSrgb(ToLinear(std::uint8_t(s))) == s);
    std::array<style::Rgba, 8> tiers{};
    tiers[4] = {220, 37, 245};
    const Rgb epic = Glow(4, tiers);
    assert(epic.b > epic.r && epic.r > epic.g * 10);
    assert(std::fabs(epic.b - ToLinear(245) * kGlow) < 1e-6f);
    const Rgb unknown = Glow(-1, tiers);
    assert(std::fabs(unknown.r - ToLinear(style::color::kTextSoft.r) * kGlow) < 1e-6f);

    // config (#135, #133): defaults = the maintainer's table
    const Config d = Defaults();
    assert(kCurated > 0 && kCurated < kTemplateCount && !d.glow.on && std::string(kTemplates[d.fly.tpl].name) == "fx_fireFlies");
    const std::size_t count[8] = {0, 0, 1, 2, 4, 4, 6, 10};
    for (int g = 0; g < 8; g++) {
        const std::vector<Layer> p = Plan(d, g, false);
        assert(p.size() == count[g]);
        for (const Layer& l : p) assert(l.tpl == d.fly.tpl && l.scale == 0.6f && l.bright == 10.0f);
    }
    Config e = d;  // glow switched on in the editor: one glow component first
    e.glow.on = true;
    assert(Plan(e, 3, false).size() == 3 && Plan(e, 3, false)[0].tpl == e.glow.tpl);
    {  // edits: off, min grade, copies, height
        Config c = d;
        assert(Plan(c, 0, false).empty() && Plan(c, 4, false).size() == 4);
        c.fly.minGrade = 6;
        assert(Plan(c, 4, false).empty() && Plan(c, 6, false).size() == 6);
        c.fly.copies[6] = 99;  // capped
        assert(Plan(c, 6, false).size() == std::size_t(kMaxCopies));
        c.glow.on = true, c.glow.height = 10;
        assert(Plan(c, 0, false)[0].dz == kLiftItem + 10 && !SameSpawn(Plan(c, 0, false), Plan(d, 0, false)));
        Config b = d;
        b.glow.bright = 3;  // tint only: no respawn
        assert(SameSpawn(Plan(b, 3, false), Plan(d, 3, false)) && !(b == d));
    }
    {  // placed at the pile, re-placed only when it moved
        const std::vector<Layer> a = Plan(d, 3, false, 100, 200, 300), b = Plan(d, 3, false, 103, 200, 300), c2 = Plan(d, 3, false, 120, 200, 300);
        assert(a[0].x == 100 && a[0].z == 300 && SameSpawn(a, b) && !SameSpawn(a, c2));
    }
    assert(Contains("hp_mag_alchemyOrb_fireflies", "FIREFL") && !Contains("fx_fireFlies", "butterfly") && Contains("x", ""));
    assert(Emissive(4, tiers, true, 2).b == 2 * epic.b && Emissive(4, tiers, false, 1).r == Emissive(4, tiers, false, 1).b);
    const std::string line = Format(d);
    assert(line.find("glow on=0") != std::string::npos && line.find("tpl=fx_fireFlies") != std::string::npos);
    assert(line.find("copies=[0,0,1,2,4,4,6,10]") != std::string::npos);
    std::puts("idle-loot ok");
}

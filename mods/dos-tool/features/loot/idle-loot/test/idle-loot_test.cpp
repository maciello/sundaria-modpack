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

    // picker: curated list leads (the default template first), search is case-insensitive, brightness scales the tint
    assert(kCurated > 0 && kCurated < kTemplateCount && std::wstring(kTemplates[0].path) == kTemplate);
    assert(Contains("hp_mag_alchemyOrb_fireflies", "FIREFL") && !Contains("fx_fireFlies", "butterfly") && Contains("x", ""));
    Tuning tn;
    assert(Emissive(4, tiers, tn).b == epic.b);
    tn.bright = 2;
    assert(std::fabs(Emissive(4, tiers, tn).b - 2 * epic.b) < 1e-3f);
    tn.byGrade = false;
    assert(Emissive(4, tiers, tn).r == Emissive(4, tiers, tn).b);
    Tuning t0, b2 = t0;
    b2.bright = 3, b2.byGrade = false;
    assert(!NeedsRespawn(t0, b2));
    b2.height = 5;
    assert(NeedsRespawn(t0, b2));
    assert(Format(t0).find("template=hp_mag_alchemyOrb_fireflies scale=1.00") == 0);
    std::puts("idle-loot ok");
}

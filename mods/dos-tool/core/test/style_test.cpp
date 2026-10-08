// just test
#include "style.hpp"
#include "../../third_party/imgui/imgui.h"  // IM_COL32 only (macro, no link)
#include <cassert>
#include <cmath>
#include <cstdio>

using namespace style;

static double Lin(int c) { const double v = c / 255.0; return v <= 0.04045 ? v / 12.92 : std::pow((v + 0.055) / 1.055, 2.4); }
static double Luma(Rgba c) { return 0.2126 * Lin(c.r) + 0.7152 * Lin(c.g) + 0.0722 * Lin(c.b); }
static double Contrast(Rgba x, Rgba y) {  // WCAG 2.x
    const double a = Luma(x), b = Luma(y);
    return (std::max(a, b) + 0.05) / (std::min(a, b) + 0.05);
}
static double DeltaE(Rgba x, Rgba y) {  // Euclidean distance in OKLab (Ottosson)
    auto lab = [](Rgba c, double o[3]) {
        const double r = Lin(c.r), g = Lin(c.g), b = Lin(c.b);
        const double l = std::cbrt(0.4122214708 * r + 0.5363325363 * g + 0.0514459929 * b);
        const double m = std::cbrt(0.2119034982 * r + 0.6806995451 * g + 0.1073969566 * b);
        const double s = std::cbrt(0.0883024619 * r + 0.2817188376 * g + 0.6299787005 * b);
        o[0] = 0.2104542553 * l + 0.7936177850 * m - 0.0040720468 * s;
        o[1] = 1.9779984951 * l - 2.4285922050 * m + 0.4505937099 * s;
        o[2] = 0.0259040371 * l + 0.7827717662 * m - 0.8086757660 * s;
    };
    double p[3], q[3];
    lab(x, p); lab(y, q);
    return std::sqrt((p[0] - q[0]) * (p[0] - q[0]) + (p[1] - q[1]) * (p[1] - q[1]) + (p[2] - q[2]) * (p[2] - q[2]));
}

int main() {
    // Packing matches ImGui.
    static_assert(Pack({1, 2, 3, 4}) == IM_COL32(1, 2, 3, 4));
    static_assert(Pack(color::kInk, 0.85f) == IM_COL32(20, 12, 8, 217));
    static_assert(Mix(color::kInk, color::kText, 1).r == 255 && Mix(color::kInk, color::kText, 0).g == 12);

    // One colour per element, in combat::Element order.
    static_assert(element::kCount == int(combat::Element::Environment) + 1);
    static_assert(rarity::kCount == 8);  // EItemGrade NewEnumerator0..7

    // Every number fill reads on its ink outline (WCAG AA 4.5:1) and no two fills can be confused (OKLab ΔE ≥ 0.12).
    Rgba fills[element::kCount + 2];
    for (int i = 0; i < element::kCount; i++) fills[i] = element::kColor[i];
    fills[element::kCount] = color::kHeal;
    fills[element::kCount + 1] = color::kTaken;
    for (const Rgba& f : fills) assert(Contrast(f, color::kInk) >= 4.5);
    for (int i = 0; i < element::kCount + 2; i++)
        for (int j = i + 1; j < element::kCount + 2; j++) {
            if (DeltaE(fills[i], fills[j]) < 0.12) std::printf("fills %d %d too close: %.3f\n", i, j, DeltaE(fills[i], fills[j]));
            assert(DeltaE(fills[i], fills[j]) >= 0.12);
        }
    // The stack counter must not read as an element (it once was gold = Holy).
    assert(DeltaE(color::kTextSoft, element::Of(combat::Element::Holy)) >= 0.12);
    for (Rgba t : {color::kText, color::kTextSoft, color::kAccent}) assert(Contrast(t, color::kInk) >= 7.0);
    assert(Contrast(color::kTextMuted, color::kPanel) >= 4.5);
    for (Rgba badge : {color::kGood, color::kSell}) assert(Contrast(color::kInk, badge) >= 7.0);  // ink text on badge pills

    // Glowing rarity tiers: distinct from each other, visible on the ink edge (non-text 3:1).
    for (int i = rarity::kGlowFrom; i < rarity::kCount; i++) {
        assert(Contrast(rarity::Of(i), color::kInk) >= 3.0);
        for (int j = i + 1; j < rarity::kCount; j++) assert(DeltaE(rarity::Of(i), rarity::Of(j)) >= 0.12);
    }
    assert(rarity::Of(-1).r == rarity::kTier[0].r && rarity::Of(99).r == rarity::kTier[7].r);

    // Type scale ascends; stack cap lies in the number range.
    static_assert(type::kXs < type::kSm && type::kSm < type::kMd && type::kMd < type::kLg && type::kLg < type::kXl);
    static_assert(type::kXl <= type::kAtlasPx);
    static_assert(type::kNumberMin < type::kStackCap && type::kStackCap < type::kNumberMax);

    // Curves start at 0, end at 1; OutBack overshoots.
    using ease::Curve;
    for (Curve c : {Curve::Linear, Curve::OutCubic, Curve::InQuad, Curve::InOutCubic, Curve::OutBack}) {
        assert(std::fabs(ease::Apply(c, 0)) < 1e-5f && std::fabs(ease::Apply(c, 1) - 1) < 1e-5f);
    }
    assert(ease::Apply(Curve::OutBack, 0.6f, motion::kPop.k) > 1.0f);
    assert(stroke::Outline(12) == 1.5f && stroke::Outline(64) == 4.0f);
    std::puts("ok");
}

#pragma once
#include <algorithm>
#include <cstdint>
#include "element.hpp"

// Design tokens. Every visual reads its colours, sizes and timings from here.
// Spec: .claude/skills/sundaria-modding/references/design-system.md. SDK- and ImGui-free.
namespace style {
    struct Rgba { std::uint8_t r, g, b, a = 255; };

    // Same bit layout as IM_COL32 (RGBA, R in the low byte). alpha multiplies the token's own alpha.
    constexpr std::uint32_t Pack(Rgba c, float alpha = 1.0f) {
        const float a = std::clamp(c.a * alpha, 0.0f, 255.0f);
        return std::uint32_t(c.r) | std::uint32_t(c.g) << 8 | std::uint32_t(c.b) << 16 | std::uint32_t(a + 0.5f) << 24;
    }
    constexpr Rgba Mix(Rgba x, Rgba y, float t) {
        auto m = [t](std::uint8_t p, std::uint8_t q) { return std::uint8_t(p + (q - p) * std::clamp(t, 0.0f, 1.0f) + 0.5f); };
        return {m(x.r, y.r), m(x.g, y.g), m(x.b, y.b), m(x.a, y.a)};
    }

    namespace color {
        constexpr Rgba kInk{20, 12, 8};            // outlines, badge discs: warm near-black
        constexpr Rgba kShadow{0, 0, 0, 115};      // drop shadows under cards/panels
        constexpr Rgba kText{255, 255, 255};       // primary text, Physical numbers
        constexpr Rgba kTextSoft{235, 225, 205};   // secondary: stack counter, subtitles
        constexpr Rgba kTextMuted{165, 155, 145};  // disabled, hints
        constexpr Rgba kAccent{255, 166, 69};      // chrome accent = the game's accent orange (spinner values). Never on numbers
        constexpr Rgba kGameText{239, 239, 239};   // the game's button text (0.937)
        constexpr Rgba kGameHighlight{252, 255, 0}; // the game's counts / active marks
        constexpr Rgba kGamePositive{21, 200, 0};   // the game's "better" stat-compare colour (WidgetItemSingleStat_C::ComparisonColorPositive)
        constexpr Rgba kGameNegative{248, 42, 0};   // the game's "worse" stat-compare colour (ComparisonColorNegative, hex F82A00)
        constexpr Rgba kPanel{18, 15, 22, 214};    // menu/card plate
        constexpr Rgba kPanelEdge{255, 166, 69, 90};
        constexpr Rgba kTrack{45, 32, 32, 153};    // empty bar track
        constexpr Rgba kHpFill{215, 45, 40};
        constexpr Rgba kHpSheen{255, 140, 120, 90};
        constexpr Rgba kHpChip{255, 248, 235, 230};
        constexpr Rgba kHeal{90, 255, 170};
        constexpr Rgba kTaken{255, 80, 70};
        constexpr Rgba kTargetEnemy{255, 120, 90};  // cast mode: the enemy an ability would hit (ally = kHeal)
        constexpr Rgba kAim{255, 232, 160};         // cast mode: ground circle of an area ability
        constexpr Rgba kGood{120, 230, 120};       // suggestion badge: upgrade
        constexpr Rgba kSell{240, 170, 60};        // suggestion badge: sell
    }

    namespace element {  // indexed by combat::Element
        constexpr Rgba kColor[] = {
            {255, 255, 255},  // Physical
            {255, 140, 40},   // Fire
            {130, 210, 255},  // Ice
            {190, 120, 255},  // Lightning
            {255, 215, 90},   // Holy
            {165, 215, 35},   // Poison
            {225, 65, 165},   // Shadow
            {80, 140, 255},   // Arcane
            {190, 165, 130},  // Environment
        };
        constexpr int kCount = int(sizeof(kColor) / sizeof(kColor[0]));
        constexpr Rgba Of(combat::Element e) { return kColor[int(e) < kCount ? int(e) : 0]; }
    }

    namespace rarity {  // EItemGrade 0..7
        // The game's own colours: BP_ArchonClientFunctionLibrary_C::GetItemColorForGrade (`just data bp
        // /Game/Blueprints/BP_ArchonClientFunctionLibrary GetItemColorForGrade`), LinearColor -> sRGB bytes.
        constexpr Rgba kTier[] = {
            {188, 188, 188},  // 0 grey    (0.5, 0.5, 0.5)
            {255, 255, 255},  // 1 white   (1, 1, 1)
            {0, 255, 0},      // 2 green   (0, 1, 0)
            {0, 89, 255},     // 3 blue    (0, 0.1, 1)
            {220, 37, 245},   // 4 purple  (0.7168, 0.0182, 0.91)
            {255, 237, 0},    // 5 yellow  (1, 0.8431, 0): crafting / non-equipment
            {255, 16, 0},     // 6 red     (1, 0.005, 0)
            {0, 255, 255},    // 7 cyan    (0, 1, 1): eternal
        };
        constexpr int kCount = int(sizeof(kTier) / sizeof(kTier[0]));
        constexpr int kGlowFrom = 3;  // tiers below this get no idle glow/beam
        constexpr Rgba Of(int tier) { return kTier[std::clamp(tier, 0, kCount - 1)]; }
    }

    namespace type {  // px at 1080p; multiply by Ui(h)
        constexpr float Ui(float screenH) { return screenH / 1080.0f; }
        constexpr float kXs = 13, kSm = 17, kMd = 26, kLg = 42, kXl = 64;
        constexpr float kAtlasPx = 64;  // baked font size: larger text is upscaled (soft)
        constexpr float kNumberMin = 0.8f, kNumberMax = 2.4f, kStackCap = 1.6f, kHaloFrom = 1.8f;
        constexpr float kCounterRatio = 0.34f, kCounterMinPx = 12;
        constexpr float kIconRatio = 0.2f, kIconMinR = 6, kIconGlyph = 0.68f;
    }

    namespace hud {  // HUD under the character (hit pips, N > 10 bar); px at 1080p × Ui(h)
        constexpr float kBarW = 120, kBarH = 6;
        constexpr float kPipsY = 0.62f;  // row centre, × screen height
    }

    namespace space { constexpr float k1 = 2, k2 = 4, k3 = 6, k4 = 8, k5 = 12, k6 = 16, k7 = 24, k8 = 32; }
    namespace radius { constexpr float kSm = 3, kMd = 6, kLg = 10; constexpr float Pill(float h) { return h * 0.5f; } }

    namespace stroke {
        constexpr float Outline(float textPx) { return std::max(1.5f, textPx / 16.0f); }
        constexpr float kOutlineAlpha = 0.85f;
        constexpr float kGlowWidth = 3.0f;   // × Outline
        constexpr float kGlowAlpha = 0.35f;
        constexpr float kShadowDy = 1.0f / 24.0f;  // × text px
        constexpr float kBarEdge = 2;        // px at 1080p
    }

    namespace ease {
        enum class Curve : std::uint8_t { Linear, OutCubic, InQuad, InOutCubic, OutBack };
        constexpr float Apply(Curve c, float x, float k = 1.70158f) {
            x = std::clamp(x, 0.0f, 1.0f);
            const float u = 1 - x;
            switch (c) {
                case Curve::OutCubic: return 1 - u * u * u;
                case Curve::InQuad: return x * x;
                case Curve::InOutCubic: return x < 0.5f ? 4 * x * x * x : 1 - 4 * u * u * u;
                case Curve::OutBack: return 1 + (k + 1) * -u * u * u + k * u * u;
                default: return x;
            }
        }
    }

    namespace motion {
        struct Motion { float dur; ease::Curve curve; float k = 0; };
        using C = ease::Curve;
        constexpr Motion kPop{0.22f, C::OutBack, 1.7f};   // number birth; k += 1.6 × big
        constexpr Motion kRise{0.6f, C::OutCubic};
        constexpr Motion kBump{0.25f, C::InQuad, 0.35f};  // stack merge kick, k = amplitude
        constexpr Motion kTick{0.12f, C::OutCubic, 1.25f}; // counter punch, k = start scale
        constexpr Motion kFlash{0.12f, C::Linear};
        constexpr float kNumberLife = 1.4f, kFadeTail = 0.3f;  // fade = last 30 % of life, InQuad
        constexpr Motion kFadeIn{0.15f, C::OutCubic};
        constexpr Motion kFadeOut{0.4f, C::InQuad};
        constexpr Motion kBarFlash{0.18f, C::Linear};
        constexpr float kChipHold = 0.4f, kDrainPerSec = 0.8f, kLinger = 3.0f;
        constexpr Motion kPipFill{0.18f, C::OutBack, 2.0f};
        constexpr float kPulsePeriod = 1.6f;    // badge / glow breathing
        constexpr Motion kCardIn{0.45f, C::OutCubic};
        constexpr Motion kCardOut{0.35f, C::InQuad};
        constexpr float kCardHold = 2.6f;
    }

    // Draw order, back to front. WorldGlow/WorldBar: ImGui background list. WorldNumber/Hud/Cinematic: foreground
    // list (drawn above ImGui windows). Menu: the Insert window; a Cinematic hides the HUD layers while it runs.
    enum class Layer : std::uint8_t { WorldGlow, WorldBar, WorldNumber, Hud, Cinematic, Menu };
}

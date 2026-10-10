#pragma once
// SDK-free logic of the idle loot sparkle (#27, #110): a game particle system on each unlooted loot actor, coloured by
// the best item grade lying around it. Spec: references/design-system.md § Loot marker (idle sparkle).
// Assets: references/game-facts.md § loot_fx.
#include <algorithm>
#include <array>
#include <cmath>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>
#include "style.hpp"
#include "../shared/loot.hpp"
#include "sparkle-templates.hpp"

namespace idle_loot {
    // Cascade template: 5 fireflies orbiting the anchor forever, each flickering at its own random rate (material
    // dynamic parameter ThoraxFlickerRate 0.05..5): a sparse, random, continuous twinkle.
    constexpr const wchar_t* kTemplate = L"/Game/Environments/HumanProps/Magic/Particles/hp_mag_alchemyOrb_fireflies.hp_mag_alchemyOrb_fireflies";
    constexpr const wchar_t* kGlowParam = L"Emissive Thorax";  // vector param of its material fx_fireFlies_Full (MI value FFFF00)
    // emissive = grade colour (linear) × this. The template's own material instance is HDR: Emissive Thorax = (5000, 2130, 0)
    // (luminance ~2600); ×3 was ~1000× dimmer on 5-10 cm sprites = invisible (#131).
    constexpr float kGlow = 2500.0f;
    constexpr float kPileR = 150;                       // cm: loot this close shares the best grade among it
    constexpr float kLiftItem = 15, kLiftChest = 45;    // cm above the actor's root
    constexpr float kScaleItem = 2.0f, kScaleChest = 3.0f;  // component scale: orbit ≤ 20 cm, sprites 5-10 cm (× this)
    constexpr float kCull = 3000;                       // cm: cull distance of the component
    // "Preview sparkle" (Insert menu): one sparkle per grade 0..7 in a row beside the hero, removed after kPreviewFor.
    constexpr int kPreviewGrades = 8;
    constexpr float kPreviewAhead = 250, kPreviewGap = 60;  // cm: row centre in front of the hero, spacing along the row
    constexpr double kPreviewFor = 5.0;                     // s
    constexpr double kSettle = 2.0;                     // s: after a loot event, re-read loot state on every world tick this long

    // Per actor: the best known grade among the unlooted loot within kPileR (-1: none known), or kLooted.
    constexpr int kLooted = -2;
    inline std::vector<int> PileGrades(const std::vector<loot::Actor>& a) {
        // ponytail: O(n²) over the tracked loot; a dungeon streams 1-9 loot actors at a time
        std::vector<int> g(a.size(), kLooted);
        for (size_t i = 0; i < a.size(); i++) {
            if (!a[i].unlooted) continue;
            g[i] = a[i].grade;
            for (const loot::Actor& o : a) {
                const float dx = o.x - a[i].x, dy = o.y - a[i].y, dz = o.z - a[i].z;
                if (o.unlooted && dx * dx + dy * dy + dz * dz <= kPileR * kPileR) g[i] = std::max(g[i], o.grade);
            }
        }
        return g;
    }

    inline float ToLinear(std::uint8_t s) {  // sRGB byte -> linear (inverse of loot::ToSrgb)
        const float c = s / 255.0f;
        return c <= 0.04045f ? c / 12.92f : std::pow((c + 0.055f) / 1.055f, 2.4f);
    }
    struct Rgb { float r, g, b; };
    // Emissive for a grade: the game's grade colour (sRGB as its UI shows it), unknown grade (closed chest): kTextSoft.
    inline Rgb Glow(int grade, const std::array<style::Rgba, 8>& tiers) {
        const style::Rgba c = grade >= 0 && grade < 8 ? tiers[grade] : style::color::kTextSoft;
        return {ToLinear(c.r) * kGlow, ToLinear(c.g) * kGlow, ToLinear(c.b) * kGlow};
    }

    // Insert-menu tuning (#133). tpl indexes kTemplates; scale/height change the spawn (respawn), bright/byGrade only the tint.
    struct Tuning {
        std::size_t tpl = 0;
        float scale = 1;        // x kScaleItem / kScaleChest
        float bright = 1;       // x kGlow
        float height = 0;       // cm added to kLiftItem / kLiftChest
        bool byGrade = true;    // false: white
        bool operator==(const Tuning&) const = default;
    };
    inline bool NeedsRespawn(const Tuning& a, const Tuning& b) { return a.tpl != b.tpl || a.scale != b.scale || a.height != b.height; }
    inline Rgb Emissive(int grade, const std::array<style::Rgba, 8>& tiers, const Tuning& t) {
        const Rgb c = t.byGrade ? Glow(grade, tiers) : Rgb{kGlow, kGlow, kGlow};
        return {c.r * t.bright, c.g * t.bright, c.b * t.bright};
    }
    // Case-insensitive substring: the picker's search box.
    inline bool Contains(const char* hay, const char* needle) {
        for (; *hay; hay++) {
            const char *h = hay, *n = needle;
            while (*n && *h && std::tolower((unsigned char)*h) == std::tolower((unsigned char)*n)) h++, n++;
            if (!*n) return true;
        }
        return !*needle;
    }
    // "Copy values": one log line holding everything needed to make the choice the code default.
    inline std::string Format(const Tuning& t) {
        char b[200];
        std::snprintf(b, sizeof b, "template=%s scale=%.2f bright=%.2f height=%.0f byGrade=%d", kTemplates[t.tpl].name, t.scale, t.bright, t.height,
                      int(t.byGrade));
        const wchar_t* w = kTemplates[t.tpl].path;
        return std::string(b) + " path=" + std::string(w, w + std::char_traits<wchar_t>::length(w));
    }
}

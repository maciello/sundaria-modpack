#pragma once
// SDK-free settings of the idle sparkle: two layers (glow, fireflies), each with its own particle system, on/off, size and
// copies per grade, brightness, height, lowest grade (#135, #133). The Insert menu edits a Config live; Plan() turns it into
// the particle components of one loot pile. Defaults = the maintainer's spec 2026-10-10.
#include <algorithm>
#include <cctype>
#include <cstdio>
#include <string>
#include <vector>
#include "sparkle-templates.hpp"

namespace idle_loot {
    constexpr int kGrades = 8;  // EItemGrade: grey, white, green, blue, purple, yellow (crafting), red, cyan (eternal)
    constexpr const char* kGradeName[kGrades] = {"grey", "white", "green", "blue", "purple", "yellow", "red", "eternal"};
    constexpr float kLiftItem = 15, kLiftChest = 45;  // cm above the actor's root
    constexpr float kChestSize = 1.5f;                // chests: sizes x this
    constexpr float kCopySpread = 8;                  // cm: copies of one layer are stacked this far apart

    constexpr std::size_t IndexOf(const char* n) {
        for (std::size_t i = 0; i < kTemplateCount; i++) {
            const char *a = kTemplates[i].name, *b = n;
            while (*a && *a == *b) a++, b++;
            if (!*a && !*b) return i;
        }
        return 0;
    }

    struct LayerCfg {
        bool on = true;
        std::size_t tpl = 0;
        float size[kGrades] = {1, 1, 1, 1, 1, 1, 1, 1};  // x the template's own size
        int copies[kGrades] = {1, 1, 1, 1, 1, 1, 1, 1};  // 0 = none for that grade
        float bright = 1;                                // x the template's tint magnitude (idle_loot::kGlow x gain)
        float height = 0;                                // cm added to the lift
        int minGrade = 0;                                // grades below show no layer
        bool operator==(const LayerCfg&) const = default;
    };
    struct Config {
        LayerCfg glow, fly;
        bool byGrade = true;  // false: white
        bool operator==(const Config&) const = default;
    };

    // Defaults. Glow: P_ky_aura_yellow (tintable, not a trail: fx_HolyLightTrail is an anim trail, it emits nothing on static loot;
    // P_StaffGlow_01 has no colour path), size by grade. Fireflies: 0.4x, brightness x5, blue and above, two for red and eternal.
    inline Config Defaults() {
        Config c;
        c.glow.tpl = IndexOf("P_ky_aura_yellow");
        const float glow[kGrades] = {0.5f, 0.5f, 0.6f, 0.7f, 0.8f, 0.9f, 1.0f, 1.2f};
        std::copy(glow, glow + kGrades, c.glow.size);
        c.fly.tpl = IndexOf("hp_mag_alchemyOrb_fireflies");
        std::fill(c.fly.size, c.fly.size + kGrades, 0.4f);
        const int fly[kGrades] = {0, 0, 0, 1, 1, 0, 2, 2};
        std::copy(fly, fly + kGrades, c.fly.copies);
        c.fly.bright = 5.0f;
        c.fly.minGrade = 3;
        return c;
    }

    // One particle component to spawn: template, size, tint magnitude, height above the actor root.
    struct Layer {
        std::size_t tpl;
        float scale, bright, dz;
        bool SameSpawn(const Layer& o) const { return tpl == o.tpl && scale == o.scale && dz == o.dz; }  // else: respawn
    };
    // The components of one pile: grade -1 (unknown, closed chest) counts as grade 0.
    inline std::vector<Layer> Plan(const Config& c, int grade, bool chest) {
        std::vector<Layer> out;
        const int g = std::clamp(grade, 0, kGrades - 1);
        for (const LayerCfg* l : {&c.glow, &c.fly}) {
            const int n = std::clamp(l->copies[g], 0, 4);
            if (!l->on || g < l->minGrade) continue;
            for (int i = 0; i < n; i++)
                out.push_back({l->tpl, l->size[g] * (chest ? kChestSize : 1.0f), l->bright,
                               (chest ? kLiftChest : kLiftItem) + l->height + (i - (n - 1) * 0.5f) * kCopySpread});
        }
        return out;
    }
    inline bool SameSpawn(const std::vector<Layer>& a, const std::vector<Layer>& b) {
        if (a.size() != b.size()) return false;
        for (std::size_t i = 0; i < a.size(); i++)
            if (!a[i].SameSpawn(b[i])) return false;
        return true;
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

    // "Copy values": every setting in one line, to paste into Defaults().
    inline std::string Format(const Config& c) {
        std::string s = "byGrade=" + std::to_string(int(c.byGrade));
        const char* nm[2] = {"glow", "fly"};
        const LayerCfg* ls[2] = {&c.glow, &c.fly};
        for (int k = 0; k < 2; k++) {
            const LayerCfg& l = *ls[k];
            char b[400];
            int n = std::snprintf(b, sizeof b, " | %s on=%d tpl=%s bright=%g height=%g min=%d size=[", nm[k], int(l.on), kTemplates[l.tpl].name, l.bright, l.height, l.minGrade);
            for (int g = 0; g < kGrades; g++) n += std::snprintf(b + n, sizeof b - n, g ? ",%g" : "%g", l.size[g]);
            n += std::snprintf(b + n, sizeof b - n, "] copies=[");
            for (int g = 0; g < kGrades; g++) n += std::snprintf(b + n, sizeof b - n, g ? ",%d" : "%d", l.copies[g]);
            std::snprintf(b + n, sizeof b - n, "]");
            s += b;
        }
        return s;
    }
}

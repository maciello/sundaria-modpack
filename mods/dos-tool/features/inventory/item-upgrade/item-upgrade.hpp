#pragma once
// Item upgrade marks (#117), SDK-free: what one item's slot chip and detail lines say, from its DPS scores.
// Spec: references/design-system.md § Item upgrade marks. Scores: libs/dps ScoreItem / BestInSlot.
#include <cstdint>
#include <cstdio>
#include <string>
#include <string_view>
#include <vector>

namespace item_upgrade {
    constexpr float kMinGain = 1.0f;   // % DPS: smaller gains are noise, no mark
    constexpr int kNameChars = 8;      // other-hero chip: hero name cut to this many characters
    constexpr float kChipFont = 20;    // Narkisim size, UMG units (slot = 100)
    constexpr float kChipPadL = 6, kChipPadT = 2, kChipPadR = 6, kChipPadB = 0;
    constexpr float kChipInset = 4;    // from the slot's left and bottom edges

    enum class Mark : std::uint8_t { None, Upgrade, OtherHero, Downgrade };
    struct Other { std::string hero, cls; float pct = 0; bool fits = false; };  // the item for another saved hero
    struct Verdict {
        Mark mark = Mark::None;
        float pct = 0;                  // Upgrade / Downgrade: DPS change for the current hero
        std::string hero, cls;          // best other hero (any mark), "" = none above kMinGain
        float heroPct = 0;
        std::vector<std::string> bis;   // classes this item is best in slot for
    };

    // self: the current hero's score; others: every other saved hero; bis: classes it is best in slot for.
    inline Verdict Judge(float selfPct, bool selfFits, const std::vector<Other>& others, std::vector<std::string> bis) {
        Verdict v;
        v.bis = std::move(bis);
        for (const Other& o : others)
            if (o.fits && o.pct >= kMinGain && o.pct > v.heroPct) { v.hero = o.hero; v.cls = o.cls; v.heroPct = o.pct; }
        if (selfFits && selfPct >= kMinGain) { v.mark = Mark::Upgrade; v.pct = selfPct; }
        else if (!v.hero.empty()) v.mark = Mark::OtherHero;
        else if (selfFits && selfPct <= -kMinGain) { v.mark = Mark::Downgrade; v.pct = selfPct; }
        return v;
    }

    inline std::string Pct(float pct) {  // "+4.2%" / "-4.2%", from 10 % on "+12%"
        char b[16];
        const float a = pct < 0 ? -pct : pct;
        std::snprintf(b, sizeof b, a >= 9.95f ? "%c%.0f%%" : "%c%.1f%%", pct < 0 ? '-' : '+', a);
        return b;
    }
    inline std::string Cut(std::string_view s, int chars) {  // first `chars` UTF-8 code points
        size_t i = 0;
        for (int n = 0; i < s.size() && n < chars; n++)
            do i++; while (i < s.size() && (static_cast<unsigned char>(s[i]) & 0xC0) == 0x80);
        return std::string(s.substr(0, i));
    }
    inline std::string Chip(const Verdict& v) {
        return v.mark == Mark::Upgrade || v.mark == Mark::Downgrade ? Pct(v.pct) : v.mark == Mark::OtherHero ? Cut(v.hero, kNameChars) : "";
    }
    // Lines for the game's item details panel, joined by '\n'; "" = none.
    inline std::string Lines(const Verdict& v) {
        std::string o;
        auto add = [&o](const std::string& l) { o += (o.empty() ? "" : "\n") + l; };
        if (v.mark == Mark::Upgrade) add("Upgrade: " + Pct(v.pct) + " DPS");
        if (v.mark == Mark::Downgrade) add("Downgrade: " + Pct(v.pct) + " DPS");
        if (!v.hero.empty()) add("Better for " + v.hero + " (" + v.cls + "): " + Pct(v.heroPct) + " DPS");
        if (!v.bis.empty()) {
            std::string c;
            for (const std::string& b : v.bis) c += (c.empty() ? "" : ", ") + b;
            add("Best in slot: " + c);
        }
        return o;
    }

    // Item identity for the score cache: spec, level, grade and rolled stats (S has .name and .value).
    // Not the slot (moves keep the score), not ChangeID (the game rewrites it on every reorder).
    template <class S> std::uint64_t Key(int spec, int level, int grade, const std::vector<S>& stats) {
        std::uint64_t h = 1469598103934665603ull;
        auto mix = [&h](const void* p, size_t n) {
            for (size_t i = 0; i < n; i++) { h ^= static_cast<const unsigned char*>(p)[i]; h *= 1099511628211ull; }
        };
        for (int x : {spec, level, grade}) mix(&x, sizeof x);
        for (const S& s : stats) { mix(s.name.data(), s.name.size()); mix(&s.value, sizeof s.value); }
        return h;
    }
}

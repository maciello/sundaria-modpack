#pragma once
// One hero's build + equipped gear as the character snapshot YAML (#102): written by char-snapshot, read by item-upgrade. SDK-free.
#include "model.hpp"
#include <cstdio>
#include <string>
#include <string_view>
#include <vector>

namespace items::hero {
    struct Learned { std::string name; int level = 0; };
    struct BarItem { int id = 0; std::string type; bool passive = false; std::vector<int> slots; };
    struct Snapshot {
        int slot = -1;                     // hero slot of the account (0..4): the character id
        std::string name, cls;
        int level = 0, heroismLevel = 0;
        std::vector<int> heroismPoints;
        std::vector<float> primaryStats;   // FSPersistentAccountHeroSummary::PrimaryStats, unnamed in the game
        std::vector<Learned> learned;
        std::vector<BarItem> bar;
        std::vector<items::Item> equipped;
    };

    inline std::string Quote(std::string_view s) {
        std::string o = "\"";
        for (char c : s) {
            if (c == '"' || c == '\\') o += '\\';
            if (c == '\n') { o += "\\n"; continue; }
            o += c;
        }
        return o + "\"";
    }
    inline std::string Num(double v) {
        char b[32];
        std::snprintf(b, sizeof b, "%.6g", v);
        return b;
    }
    template<class T> std::string Flow(const std::vector<T>& v) {
        std::string o = "[";
        for (size_t i = 0; i < v.size(); i++) o += (i ? ", " : "") + Num(double(v[i]));
        return o + "]";
    }
    inline std::string At(const std::vector<std::string>& v, int i) {
        return i >= 0 && i < int(v.size()) && !v[i].empty() ? v[i] : std::to_string(i);
    }

    // statNames: items::io::Names::stat; equipSlotNames: items::io::Names::equipSlot.
    inline std::string Yaml(const Snapshot& s, const std::vector<std::string>& statNames, const std::vector<std::string>& equipSlotNames) {
        std::string y = "# dos-tool character snapshot (#102): written when the game saves or loads this hero\n";
        y += "slot: " + std::to_string(s.slot) + "\nname: " + Quote(s.name) + "\nclass: " + Quote(s.cls) + "\nlevel: " + std::to_string(s.level) +
             "\nheroism_level: " + std::to_string(s.heroismLevel) + "\nheroism_points: " + Flow(s.heroismPoints) +
             "\nprimary_stats: " + Flow(s.primaryStats) + "\nlearned_abilities:" + (s.learned.empty() ? " []\n" : "\n");
        for (const Learned& l : s.learned) y += "  - {name: " + Quote(l.name) + ", level: " + std::to_string(l.level) + "}\n";
        y += std::string("action_bar:") + (s.bar.empty() ? " []\n" : "\n");
        for (const BarItem& b : s.bar)
            y += "  - {id: " + std::to_string(b.id) + ", type: " + Quote(b.type) + ", passive: " + (b.passive ? "true" : "false") +
                 ", slots: " + Flow(b.slots) + "}\n";
        y += std::string("equipped:") + (s.equipped.empty() ? " []\n" : "\n");
        for (const items::Item& it : s.equipped) {
            y += "  - slot: " + std::to_string(it.slot) + "\n    equip_slot: " + Quote(it.equipSlot < 0 ? "" : At(equipSlotNames, it.equipSlot)) +
                 "\n    name: " + Quote(it.name) + "\n    spec: " + std::to_string(it.specId) + "\n    kind: " + items::kKindName[int(it.kind)] +
                 "\n    type: " + Quote(it.typeName) + "\n    grade: " + std::to_string(it.grade) + "\n    level: " + std::to_string(it.level) +
                 "\n    stats:" + (it.stats.empty() ? " {}\n" : "\n");
            for (const items::Stat& st : it.stats) y += "      " + items::StatName(statNames, st.type) + ": " + Num(st.value) + "\n";
        }
        return y;
    }
}

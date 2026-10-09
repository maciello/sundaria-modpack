#pragma once
// One hero's build + equipped gear as the character snapshot YAML (#102): written by char-snapshot, read by item-upgrade. SDK-free.
#include "model.hpp"
#include <cstdio>
#include <cstdlib>
#include <utility>
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

    // ---- reading it back (item-upgrade: the other heroes' builds) ----
    struct Gear {  // an equipped item of a snapshot, names as written
        int slot = -1, spec = 0, grade = 0, level = 0;
        std::string equipSlot, name, kind, type;
        std::vector<std::pair<std::string, float>> stats;
    };
    struct Read {
        int slot = -1, level = 0;
        std::string name, cls;
        std::vector<float> primaryStats;
        std::vector<Learned> learned;
        std::vector<Gear> equipped;
    };
    inline std::string Unquote(std::string_view v) {
        if (v.size() < 2 || v.front() != '"') return std::string(v);
        std::string o;
        for (size_t i = 1; i + 1 < v.size(); i++) {
            if (v[i] != '\\' || i + 2 >= v.size()) { o += v[i]; continue; }
            const char c = v[++i];
            o += c == 'n' ? '\n' : c;
        }
        return o;
    }
    inline std::vector<float> Floats(std::string_view v) {  // "[1, 2.5]"
        std::vector<float> out;
        const std::string s(v.substr(v.find('[') == std::string_view::npos ? v.size() : v.find('[') + 1));
        for (const char* p = s.c_str(); *p && *p != ']';) {
            char* end = nullptr;
            const float f = std::strtof(p, &end);
            if (end == p) { p++; continue; }
            out.push_back(f);
            p = end;
        }
        return out;
    }
    // Parses what Yaml() writes, line by line; anything else is skipped. slot -1 = not a snapshot.
    inline Read Parse(std::string_view y) {
        Read r;
        enum { Top, Learn, Equip, Stats } at = Top;
        while (!y.empty()) {
            const size_t nl = y.find('\n');
            std::string_view line = y.substr(0, nl);
            y = nl == std::string_view::npos ? std::string_view{} : y.substr(nl + 1);
            if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
            const size_t indent = line.find_first_not_of(' ');
            if (indent == std::string_view::npos || line[indent] == '#') continue;
            line.remove_prefix(indent);
            const bool item = line.starts_with("- ");
            if (item) line.remove_prefix(2);
            const size_t colon = line.find(": ");
            const std::string_view key = line.substr(0, colon == std::string_view::npos ? line.find(':') : colon);
            const std::string_view val = colon == std::string_view::npos ? std::string_view{} : line.substr(colon + 2);
            const int n = std::atoi(std::string(val).c_str());
            if (indent == 0) {
                at = key == "learned_abilities" ? Learn : key == "equipped" ? Equip : Top;
                if (key == "slot") r.slot = n;
                else if (key == "name") r.name = Unquote(val);
                else if (key == "class") r.cls = Unquote(val);
                else if (key == "level") r.level = n;
                else if (key == "primary_stats") r.primaryStats = Floats(val);
                continue;
            }
            if (at == Learn && item && line.starts_with("{name: ")) {  // {name: "X", level: N}
                const size_t lv = line.rfind(", level: ");
                if (lv != std::string_view::npos)
                    r.learned.push_back({Unquote(line.substr(7, lv - 7)), std::atoi(std::string(line.substr(lv + 9)).c_str())});
                continue;
            }
            if (at == Stats && indent >= 6 && !r.equipped.empty()) {
                r.equipped.back().stats.push_back({std::string(key), std::strtof(std::string(val).c_str(), nullptr)});
                continue;
            }
            if (at != Equip && at != Stats) continue;
            if (item) r.equipped.emplace_back();
            if (r.equipped.empty()) continue;
            Gear& g = r.equipped.back();
            at = key == "stats" ? Stats : Equip;
            if (key == "slot") g.slot = n;
            else if (key == "equip_slot") g.equipSlot = Unquote(val);
            else if (key == "name") g.name = Unquote(val);
            else if (key == "spec") g.spec = n;
            else if (key == "kind") g.kind = std::string(val);
            else if (key == "type") g.type = Unquote(val);
            else if (key == "grade") g.grade = n;
            else if (key == "level") g.level = n;
        }
        return r;
    }
}

// StatWeights (affix unit): % DPS from one affix of each stat as it rolls on an item of each slot the class wears
// (item level = build level, given grade, value exact), with the chance the stat spawns on that slot's items.
#include "items.hpp"
#include "prepared.hpp"
#include <algorithm>
#include <map>

namespace dps {
    using namespace detail;
    using namespace layout;

    std::vector<StatWeight> Model::StatWeights(const Build& build, const Scenario& scenario, int grade) const {
        const Tables& t = tables();
        std::vector<StatWeight> out;
        auto p = Prepare(build, scenario);
        if (p->best < 0 || grade < 0 || grade >= int(t.grades.size())) return out;
        const Mode& m = p->modes[p->best];
        const std::string& g = t.grades[grade];
        const float d0 = Eval(m.c, m.row);
        if (d0 <= 0.f) return out;
        const auto tags = items::ClassTags(t, build.cls);
        const std::string armor = items::ClassArmorType(t, build.cls);
        const bool twoH = m.main && m.main->equipSlot == "WeaponDoubleHanded";
        const std::string wslot = twoH ? "WeaponDoubleHanded" : "WeaponAny";
        std::vector<std::string> slots;
        for (const char* s : {"Head", "Body", "Hands", "Legs", "Feet", "Back", "Shoulders", "Belt", "Bracer", "Neck", "Ring", "Trinket", "Crown"})
            if (tags.count(s)) slots.push_back(s);
        slots.push_back(wslot);
        if (!twoH && tags.count("WeaponLeft")) slots.push_back("WeaponLeft");
        int cur = kElements;   // weapon element in use; only an earlier element (enum order) would take over
        for (int e = 0; e < kElements; ++e) if (m.row.elem[e] > 1e-6f) { cur = e; break; }

        struct Cell { float p = 0, value = 0, pct = 0; };
        std::map<std::string, std::map<std::string, Cell>> rows;
        for (const std::string& slot : slots) {
            auto tag = tags.find(slot);
            if (tag == tags.end()) tag = tags.find("WeaponAny");
            if (tag == tags.end()) continue;
            const bool weapon = slot == wslot || (slot == "WeaponLeft" && m.off);
            const std::string wt = slot == wslot ? (m.main ? m.main->weaponType : "") : (m.off ? m.off->weaponType : "");
            for (const auto& [stat, chance] : items::SpawnChance(t, tag->second, g)) {
                Cell& cell = rows[stat][slot];
                cell.p = chance;
                cell.value = items::CraftValue(t, stat, slot, build.level, g, weapon ? "" : armor, weapon ? wt : "");
                Item it;
                it.stats.push_back({stat, cell.value});
                const Sparse s = ToSparse(m.c, it);
                Row r = m.row;
                bool reads = !s.attr.empty();
                for (int e = 0; e < kElements; ++e)
                    if (s.elem[e] > 0.f && e < cur) { r.elem = {}, r.elem[e] = s.elem[e], reads = true; }
                if (!reads) continue;
                r.Add({s.attr, {}, {}});
                cell.pct = (Eval(m.c, r) / d0 - 1.f) * 100.f;
            }
        }
        for (const auto& [stat, bySlot] : rows) {
            StatWeight w;
            w.stat = stat;
            float best = -1e9f, expected = 0.f;
            for (const auto& [slot, c] : bySlot) {
                expected += c.p * c.pct;
                if (c.pct > best) best = c.pct, w.bestSlot = slot, w.value = c.value;
            }
            w.pctBestSlot = best;
            w.pctExpectedPerItem = expected / float(slots.size());
            out.push_back(w);
        }
        std::sort(out.begin(), out.end(), [](const StatWeight& a, const StatWeight& b) {
            return a.pctBestSlot != b.pctBestSlot ? a.pctBestSlot > b.pctBestSlot : a.stat < b.stat;
        });
        return out;
    }
}

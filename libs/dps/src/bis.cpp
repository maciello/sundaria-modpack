// BestInSlot: per weapon type among the candidates, coordinate ascent over equip positions (each position takes the
// candidate that maximises DPS given the others) until nothing improves. Not certified optimal (local optimum).
#include "prepared.hpp"
#include <set>

namespace dps {
    using namespace detail;
    using namespace layout;

    namespace {
        struct Position { std::string equipSlot; std::vector<int> pool; int chosen = -1; bool required = false, off = false; };

        struct Ascent {
            const Tables& t;
            const Build& b;
            const Scenario& sc;
            std::span<const Item> cands;
            compile::Compiled c;
            std::vector<Sparse> sp, spOff;   // candidate stats in this row layout (spOff: in the off hand)
            std::vector<Position> pos;
            Row row;

            const Sparse& S(const Position& q, int i) const { return q.off ? spOff[i] : sp[i]; }
            bool Compile(const std::string& weapon, const std::string& offhand) {
                compile::Params p;
                p.cls = b.cls, p.weapon = weapon, p.offhand = offhand, p.level = b.level, p.primary = Primary(t, b);
                p.learned = &b.abilities, p.abilityLevel = b.abilityLevel, p.heroism = &b.heroism, p.scenario = &sc;
                std::string err;
                c = compile::Compile(t, p, err);
                if (!err.empty()) return false;
                sp.assign(cands.size(), {}), spOff.assign(cands.size(), {});
                std::vector<char> done(cands.size() * 2, 0);
                for (const Position& q : pos)
                    for (int i : q.pool)
                        if (!done[i * 2 + q.off]) done[i * 2 + q.off] = 1, (q.off ? spOff : sp)[i] = ToSparse(c, cands[i], q.off);
                row.x = c.base, row.elem = {};
                for (const Position& q : pos) if (q.chosen >= 0) row.Add(S(q, q.chosen));
                return true;
            }
            float Run(int offPos, const std::string& weapon) {
                float cur = Eval(c, row);
                for (int sweep = 0; sweep < 8; ++sweep) {
                    bool improved = false;
                    for (int qi = 0; qi < int(pos.size()); ++qi) {
                        Position& q = pos[qi];
                        std::set<int> used;
                        for (const Position& o : pos) if (&o != &q && o.chosen >= 0) used.insert(o.chosen);
                        if (q.chosen >= 0) row.Add(S(q, q.chosen), -1.f);
                        auto score = [&](int i) {
                            if (i >= 0) row.Add(S(q, i));
                            const float e = Eval(c, row);
                            if (i >= 0) row.Add(S(q, i), -1.f);
                            return e;
                        };
                        int pick = q.chosen;
                        float best = q.chosen >= 0 || !q.required ? score(q.chosen) : -1.f;
                        auto offer = [&](int i) {
                            const float e = score(i);
                            if (e > best * (1.f + 1e-6f)) best = e, pick = i;
                        };
                        if (!q.required && q.chosen >= 0) offer(-1);
                        for (int i : q.pool)
                            if (i != q.chosen && !used.count(i)) offer(i);
                        if (pick >= 0) row.Add(S(q, pick));
                        if (pick != q.chosen) {
                            const std::string before = q.chosen >= 0 ? AnimOf(t, &cands[q.chosen]) : std::string();
                            q.chosen = pick, improved = true;
                            // off-hand type decides shield/orb abilities: compile again when it changes
                            if (qi == offPos && AnimOf(t, pick >= 0 ? &cands[pick] : nullptr) != before)
                                Compile(weapon, AnimOf(t, pick >= 0 ? &cands[pick] : nullptr));
                        }
                        cur = Eval(c, row);
                    }
                    if (!improved) break;
                }
                return cur;
            }
        };
    }

    BestInSlotResult Model::BestInSlot(const Build& hero, const Scenario& scenario, std::span<const Item> cands) const {
        const Tables& t = tables();
        BestInSlotResult res;
        std::map<std::string, std::vector<int>> armor;
        std::vector<int> oneH, left;
        std::map<std::pair<std::string, bool>, std::vector<int>> mains;   // (weapon type, two-handed) -> candidates
        for (int i = 0; i < int(cands.size()); ++i) {
            const Item& it = cands[i];
            if (it.equipSlot.empty() || !CanUse(t, hero, it)) continue;
            if (!IsWeaponSlot(it.equipSlot)) { armor[it.equipSlot].push_back(i); continue; }
            if (!t.weaponStats.count(it.weaponType)) continue;
            if (it.equipSlot == "WeaponLeft") { left.push_back(i); continue; }
            const bool twoH = it.equipSlot == "WeaponDoubleHanded";
            mains[{it.weaponType, twoH}].push_back(i);
            if (!twoH) oneH.push_back(i);
        }
        if (mains.empty()) { res.error = "no candidate weapon"; return res; }
        Build b = hero;
        b.equipped.clear();
        for (const auto& [key, pool] : mains) {
            Ascent a{t, b, scenario, cands, {}, {}, {}, {}};
            a.pos.push_back({key.second ? "WeaponDoubleHanded" : "WeaponAny", pool, pool[0], true});
            int offPos = -1;
            if (!key.second) {
                std::vector<int> off = left;
                off.insert(off.end(), oneH.begin(), oneH.end());
                offPos = int(a.pos.size());
                a.pos.push_back({"WeaponLeft", off, -1, false, true});
            }
            for (const auto& [slot, pool2] : armor)
                for (int k = 0; k < Capacity(slot); ++k) a.pos.push_back({slot, pool2, -1, false});
            if (!a.Compile(key.first, "")) continue;
            const float d = a.Run(offPos, key.first);
            if (d <= res.dps) continue;
            res.dps = d, res.weaponType = a.c.animType, res.picks.clear();
            for (const Position& q : a.pos) res.picks.push_back({q.equipSlot, q.chosen});
        }
        if (res.picks.empty()) res.error = "no weapon type compiled for " + hero.cls;
        return res;
    }

    BestInSlotResult Model::BestInSlot(std::string_view cls, int level, const Scenario& scenario, std::span<const Item> cands) const {
        Build b;
        b.cls = std::string(cls), b.level = level;
        return BestInSlot(b, scenario, cands);
    }
}

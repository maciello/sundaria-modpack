// ScoreItem: DPS change if an item replaced one equipped item (or filled an empty slot), best fitting slot wins.
// Armor/trinkets: a row delta per weapon set + one kernel call each. Weapons: the same, unless the weapon type or the
// off-hand type changes; then that set is compiled again (abilities depend on the weapon).
#include "prepared.hpp"
#include <algorithm>

namespace dps {
    using namespace detail;
    using namespace layout;

    namespace {
        // DPS of the build with weapon set `mi` (-1 = a new set) replaced by w; the hero uses the better set
        float WithSet(const Tables& t, const Prepared& p, int mi, WeaponSet w) {
            float other = 0.f;
            for (int i = 0; i < int(p.modes.size()); ++i)
                if (i != mi) other = std::max(other, p.modes[i].dps);
            float d = 0.f;
            const Mode* m = mi >= 0 ? &p.modes[mi] : nullptr;
            if (m && m->main && w.main->weaponType == m->main->weaponType && AnimOf(t, w.off) == m->c.offhand) {
                Row r = m->row;
                if (m->main != w.main) r.Add(ToSparse(m->c, *m->main), -1.f), r.Add(ToSparse(m->c, *w.main));
                if (m->off != w.off) {
                    if (m->off) r.Add(ToSparse(m->c, *m->off, true), -1.f);
                    if (w.off) r.Add(ToSparse(m->c, *w.off, true));
                }
                d = Eval(m->c, r);
            } else {
                std::string err;
                Mode n = MakeMode(t, p.build, p.scenario, p.common, w, err);
                d = err.empty() ? n.dps : 0.f;
            }
            return std::max(other, d);
        }
        int SetOf(const Prepared& p, bool twoHanded) {
            for (int i = 0; i < int(p.modes.size()); ++i)
                if (p.modes[i].main && (p.modes[i].main->equipSlot == "WeaponDoubleHanded") == twoHanded) return i;
            return -1;
        }
    }

    ItemScore Model::ScoreItem(const Prepared& p, const Item& item, int slot) const {
        const Tables& t = tables();
        ItemScore s;
        if (p.best < 0 || item.equipSlot.empty()) return s;
        for (const Item& e : p.build.equipped)   // the hero wears this very item: no change
            if (e.slot >= 0 && e.slot == item.slot && e.spec == item.spec && e.level == item.level && e.grade == item.grade &&
                e.stats.size() == item.stats.size() &&
                std::equal(e.stats.begin(), e.stats.end(), item.stats.begin(), [](const Stat& x, const Stat& y) { return x.name == y.name && x.value == y.value; }))
                return {0.f, p.dps(), e.slot, true};
        auto consider = [&](float dps, int replaces) {
            if (slot >= 0 && replaces != slot) return;
            if (!s.fits || dps > s.dps) s.fits = true, s.dps = dps, s.replacesSlot = replaces;
        };
        if (!IsWeaponSlot(item.equipSlot)) {
            std::vector<const Item*> same;
            for (const Item* it : p.common)
                if (it->equipSlot == item.equipSlot) same.push_back(it);
            std::vector<Sparse> sp(p.modes.size());   // the item in each set's row layout
            for (size_t i = 0; i < p.modes.size(); ++i) sp[i] = ToSparse(p.modes[i].c, item);
            auto with = [&](const Item* old) {
                float best = 0.f;
                for (size_t i = 0; i < p.modes.size(); ++i) {
                    const Mode& m = p.modes[i];
                    Row r = m.row;
                    if (old) r.Add(ToSparse(m.c, *old), -1.f);
                    r.Add(sp[i]);
                    best = std::max(best, Eval(m.c, r));
                }
                return best;
            };
            for (const Item* old : same) consider(with(old), old->slot);
            if (int(same.size()) < Capacity(item.equipSlot)) consider(with(nullptr), -1);
        } else if (t.weaponStats.count(item.weaponType)) {
            const bool twoH = item.equipSlot == "WeaponDoubleHanded";
            const int mi = SetOf(p, twoH);
            const Mode* m = mi >= 0 ? &p.modes[mi] : nullptr;
            if (twoH) consider(WithSet(t, p, mi, {&item, nullptr}), m ? m->main->slot : -1);
            else if (item.equipSlot == "WeaponLeft") {
                if (m) consider(WithSet(t, p, mi, {m->main, &item}), m->off ? m->off->slot : -1);
            } else if (!m) consider(WithSet(t, p, -1, {&item, nullptr}), -1);
            else {
                consider(WithSet(t, p, mi, {&item, m->off}), m->main->slot);
                if (!m->off || m->off->equipSlot != "WeaponLeft")   // second one-hander in the other hand
                    consider(WithSet(t, p, mi, {m->main, &item}), m->off ? m->off->slot : -1);
            }
        }
        const float cur = p.dps();
        if (s.fits && cur > 0.f) s.deltaPct = (s.dps / cur - 1.f) * 100.f;
        return s;
    }

    ItemScore Model::ScoreItem(const Build& build, const Scenario& scenario, const Item& item, int slot) const {
        return ScoreItem(*Prepare(build, scenario), item, slot);
    }
}

// Model: tables in, Prepare (build -> weapon sets), Dps with breakdown.
#include "prepared.hpp"
#include <algorithm>
#include <set>

namespace dps {
    using namespace layout;

    namespace detail {
        Sparse ToSparse(const compile::Compiled& c, const Item& it, bool offhand) {
            Sparse s;
            for (const Stat& st : it.stats) {
                const int a = c.Attr(compile::Lower(st.name));
                if (offhand && (a == I_WD || a <= -2)) continue;
                if (a >= 0) s.attr.push_back({a, st.value});
                else if (a <= -2) s.elem[-a - 2] += st.value;
                else s.ignored.push_back(st.name);
            }
            return s;
        }
        void Row::Add(const Sparse& s, float sign) {
            for (const auto& [a, v] : s.attr) x[a] += sign * v;
            for (int e = 0; e < kElements; ++e) elem[e] += sign * s.elem[e];
        }
        float Eval(const compile::Compiled& c, const Row& r, kernel::Detail* d) {
            float x[I_AB + 2 * kMaxAbilities];
            std::copy(r.x.begin(), r.x.end(), x);
            x[I_WEL] = 0, x[I_WELIDX] = 0;
            for (int e = 0; e < kElements; ++e)
                if (r.elem[e] > 1e-6f) { x[I_WEL] = r.elem[e], x[I_WELIDX] = float(e); break; }
            return kernel::Eval(c.Ctx(), x, d);
        }
        bool IsWeaponSlot(const std::string& s) {
            return s == "WeaponAny" || s == "WeaponLeft" || s == "WeaponRight" || s == "WeaponDoubleHanded";
        }
        int Capacity(const std::string& s) { return s == "Ring" || s == "Trinket" ? 2 : 1; }   // char snapshot: 2 rings, 2 trinkets
        std::array<float, 6> Primary(const Tables& t, const Build& b) {
            std::array<float, 6> p = t.primaryDefault;
            for (size_t i = 0; i < 6 && i < b.primary.size(); ++i) p[i] = b.primary[i];
            return p;
        }
        std::string AnimOf(const Tables& t, const Item* it) {
            if (!it) return {};
            auto w = t.weaponStats.find(it->weaponType);
            return w == t.weaponStats.end() ? std::string() : w->second.animationType;
        }
        bool CanUse(const Tables& t, const Build& b, const Item& it) {
            if (it.level > b.level) return false;
            const RangedAttack& r = t.rangedAttack;
            if (r.ability.empty() || !IsWeaponSlot(it.equipSlot)) return true;
            const std::string anim = AnimOf(t, &it);
            if (std::find(r.types.begin(), r.types.end(), anim) == r.types.end()) return true;
            auto c = t.abilities.find(b.cls);
            return c != t.abilities.end() &&
                   std::any_of(c->second.begin(), c->second.end(), [&](const Ability& a) { return a.name == r.ability; });
        }
        Mode MakeMode(const Tables& t, const Build& b, const Scenario& sc, const std::vector<const Item*>& common, WeaponSet w,
                      std::string& error) {
            Mode m;
            compile::Params p;
            p.cls = b.cls, p.weapon = w.main ? w.main->weaponType : std::string(), p.offhand = AnimOf(t, w.off);
            p.level = b.level, p.primary = Primary(t, b), p.learned = &b.abilities, p.abilityLevel = b.abilityLevel;
            p.heroism = &b.heroism, p.scenario = &sc;
            m.c = compile::Compile(t, p, error);
            if (!error.empty()) return m;
            m.main = w.main, m.off = w.off;
            m.row.x = m.c.base;
            for (const Item* it : common) m.row.Add(ToSparse(m.c, *it));
            if (w.main) m.row.Add(ToSparse(m.c, *w.main));
            if (w.off) m.row.Add(ToSparse(m.c, *w.off, true));
            m.dps = Eval(m.c, m.row);
            return m;
        }
    }

    struct Model::Impl { Tables t; };

    Model::Model(Tables tables) : impl_(new Impl{std::move(tables)}) {}
    Model::~Model() = default;
    const Tables& Model::tables() const { return impl_->t; }
    std::unique_ptr<Model> Model::Load(Source& source, std::string& error) {
        Tables t;
        if (!source.Load(t, error)) return nullptr;
        return std::make_unique<Model>(std::move(t));
    }

    std::shared_ptr<const Prepared> Model::Prepare(const Build& build, const Scenario& scenario) const {
        using namespace detail;
        const Tables& t = impl_->t;
        auto p = std::make_shared<Prepared>();
        p->build = build, p->scenario = scenario;
        std::vector<const Item*> twoH, oneH, left;
        for (const Item& it : p->build.equipped) {
            if (!IsWeaponSlot(it.equipSlot)) { p->common.push_back(&it); continue; }
            if (!t.weaponStats.count(it.weaponType)) continue;   // unknown weapon type: no set
            (it.equipSlot == "WeaponDoubleHanded" ? twoH : it.equipSlot == "WeaponLeft" ? left : oneH).push_back(&it);
        }
        auto bySlot = [](const Item* a, const Item* b) { return a->slot < b->slot; };
        std::sort(oneH.begin(), oneH.end(), bySlot);
        std::vector<WeaponSet> sets;
        if (!twoH.empty()) sets.push_back({twoH[0], nullptr});
        // 1H set: main hand = the WeaponAny item in the lowest equip slot (unverified which hand the game attacks with)
        if (!oneH.empty()) sets.push_back({oneH[0], !left.empty() ? left[0] : oneH.size() > 1 ? oneH[1] : nullptr});
        if (sets.empty()) { p->error = "no weapon equipped"; return p; }
        for (const WeaponSet& w : sets) {
            std::string err;
            Mode m = MakeMode(t, p->build, p->scenario, p->common, w, err);
            if (!err.empty()) { p->error = err; continue; }
            p->modes.push_back(std::move(m));
        }
        if (p->modes.empty()) return p;
        p->error.clear();
        for (int i = 0; i < int(p->modes.size()); ++i)
            if (p->best < 0 || p->modes[i].dps > p->modes[p->best].dps) p->best = i;
        return p;
    }

    Result Model::Dps(const Prepared& p) const {
        Result r;
        if (p.best < 0) { r.error = p.error.empty() ? "nothing compiled" : p.error; return r; }
        const detail::Mode& m = p.modes[p.best];
        kernel::Detail d;
        r.dps = detail::Eval(m.c, m.row, &d);
        r.weaponType = m.c.animType;
        const float T = m.c.scen[S_T];
        for (int k = 0; k < m.c.K; ++k)
            r.abilities.push_back({m.c.abilities[k].name, d.dmg[k] * d.uses[k] / T, d.dmg[k], d.cast[k], d.cd[k], d.uses[k]});
        std::stable_sort(r.abilities.begin(), r.abilities.end(), [](const AbilityDps& a, const AbilityDps& b) { return a.dps > b.dps; });
        std::set<std::string> ign;
        auto scan = [&](const Item* it) {
            if (it) for (const auto& s : detail::ToSparse(m.c, *it).ignored) ign.insert(s);
        };
        for (const Item* it : p.common) scan(it);
        scan(m.main);
        if (m.off) for (const auto& s : detail::ToSparse(m.c, *m.off, true).ignored) ign.insert(s);
        r.ignoredStats.assign(ign.begin(), ign.end());
        return r;
    }

    Result Model::Dps(const Build& build, const Scenario& scenario) const { return Dps(*Prepare(build, scenario)); }
}

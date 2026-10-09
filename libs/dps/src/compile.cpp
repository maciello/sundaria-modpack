#include "compile.hpp"
#include <algorithm>
#include <cctype>
#include <map>

namespace dps::compile {
    using namespace layout;

    namespace {
        const char* const kElementNames[] = {"Fire", "Ice", "Void", "Nature", "Lightning", "Holy", "Death", "Light"};   // EMagicDamageType
        const char* const kPrimary[] = {"Strength", "Dexterity", "Intelligence", "Wisdom", "Constitution", "Charisma"};
        const char* const kApRow[] = {"Melee", "Ranged", "Spell"};

        // montage-name tokens per animation type, best first (Mon_<Ability>_H_M_<Weapon> naming)
        const std::map<std::string, std::vector<std::string>> kWeaponTokens = {
            {"Axe", {"Axe1H", "Axe"}}, {"Club", {"Hammer1H", "Wand1H", "Club"}}, {"Crossbow", {"Crossbow1H", "Crossbow"}},
            {"Dagger", {"Dagger1H", "Dagger"}}, {"Fist", {"Fist"}}, {"FistWeapon", {"Fist"}}, {"Sword", {"Sword1H", "Sword"}},
            {"Shield", {"Shield1H"}}, {"Axe2H", {"Axe2H", "2H"}}, {"Bow2H", {"Bow2H"}}, {"Crossbow2H", {"Crossbow2H"}},
            {"Hammer2H", {"Hammer2H", "2H"}}, {"Polearm2H", {"Polearm2H", "2H"}}, {"Scythe2H", {"Hammer2H", "2H"}},
            {"Staff2H", {"Staff2H", "2H"}}, {"Sword2H", {"Sword2H", "2H"}}};
        bool WeaponToken(const std::string& s) {
            static const std::vector<std::string> extra = {"AxeDagger", "AxeDual", "DaggerDual", "DaggerSword", "Wand1H"};
            for (const auto& [k, v] : kWeaponTokens)
                if (std::find(v.begin(), v.end(), s) != v.end()) return true;
            return std::find(extra.begin(), extra.end(), s) != extra.end();
        }
        // heroism node -> attribute it adds to (per point x HeroismCurve)
        const std::map<std::string, int> kHeroismAttr = {
            {"OffensiveAttackSpeed", I_IAS}, {"OffensiveCriticalChance", I_CC}, {"OffensiveCriticalDamage", I_CD},
            {"OffensiveArmorPenetration", I_APEN}, {"OffensiveMagicPenetration", I_MPEN}, {"DefensiveCooldownReduction", I_CDR},
            {"ElementalPhysicalDamage", I_OUTPHYS}, {"ElementalFireDamage", I_DEL + 0}, {"ElementalIceDamage", I_DEL + 1},
            {"ElementalVoidDamage", I_DEL + 2}, {"ElementalNatureDamage", I_DEL + 3}, {"ElementalLightningDamage", I_DEL + 4},
            {"ElementalHolyDamage", I_DEL + 5}, {"ElementalDeathDamage", I_DEL + 6}, {"ElementalLightDamage", I_DEL + 7},
            {"AttackPowerMelee", I_AP + 0}, {"AttackPowerRanged", I_AP + 1}, {"AttackPowerMagic", I_AP + 2}};
        // class heroism node -> ability it multiplies (by display name; inferred)
        const std::map<std::string, std::string> kHeroismAbility = {
            {"ChampionSlash1", "DemonBane"}, {"ChampionShieldBash1", "Bash"}, {"ChampionMightyBlow1", "MightyBlow"},
            {"ChampionCleave1", "Cleave"}, {"RogueEviscerate1", "Eviscerate"}, {"RoguePhasePort1", "PhasePort"},
            {"RangerAimedShot1", "AimedShot"}, {"RangerParalyzingArrow1", "ParalysingShot"}, {"RangerToxicArrow1", "PoisonArrow"},
            {"ClericSmite1", "Smite"}, {"ClericHolyLight1", "HolyLight"}, {"WizardFireball1", "FireBall"}, {"WizardConeOfFire1", "FireCone"}};

        std::vector<std::string> Split(const std::string& s, char sep) {
            std::vector<std::string> v;
            size_t a = 0;
            for (size_t b; (b = s.find(sep, a)) != std::string::npos; a = b + 1) v.push_back(s.substr(a, b - a));
            v.push_back(s.substr(a));
            return v;
        }
        bool TwoDigitEnd(const std::string& s) {   // combo sections: Attack01, Attack02, ...
            const size_t n = s.size();
            return n >= 2 && std::isdigit(static_cast<unsigned char>(s[n - 1])) && std::isdigit(static_cast<unsigned char>(s[n - 2]));
        }
        bool Has(const std::vector<std::string>& v, const std::string& s) { return std::find(v.begin(), v.end(), s) != v.end(); }

        // best montage for the weapon: weapon tokens must match, earlier token = better; H_M (hero male) preferred
        const Montage* PickMontage(const Ability& a, const std::string& wt, const std::string& offhand) {
            std::vector<std::string> pref;
            if (auto it = kWeaponTokens.find(wt); it != kWeaponTokens.end()) pref = it->second;
            if (offhand == "Shield") pref.push_back("Shield1H");
            std::string key = Lower(a.name);
            while (!key.empty() && key.back() == 's') key.pop_back();
            key = key.substr(0, 8);
            const Montage* best = nullptr;
            float score = -1e9f;
            for (const Montage& m : a.montages) {
                const auto toks = Split(m.name, '_');
                int bestPref = -1;
                bool anyWeapon = false;
                for (const auto& t : toks) {
                    if (!WeaponToken(t)) continue;
                    anyWeapon = true;
                    auto p = std::find(pref.begin(), pref.end(), t);
                    if (p != pref.end() && (bestPref < 0 || p - pref.begin() < bestPref)) bestPref = int(p - pref.begin());
                }
                if (anyWeapon && bestPref < 0) continue;
                float s = anyWeapon ? float(10 - bestPref) : 1.f;
                s += Has(toks, "H") && Has(toks, "M") ? 0.5f : 0.f;
                s -= Has(toks, "BonusAbility") ? 5.f : 0.f;
                s -= Has(toks, "LM") ? 3.f : 0.f;
                s += Lower(m.name).find(key) != std::string::npos ? 3.f : 0.f;
                if (s > score) best = &m, score = s;
            }
            return best;
        }

        // cast seconds (montage time, rate 1) and hits per cast
        void CastAndHits(const Montage& m, const std::string& act, bool projectile, float& cast, float& hits, std::string& how) {
            auto h = [&](const Section& s) { return projectile && s.shots > 0 ? s.shots : s.hits; };
            auto t = [](const Section& s) { return s.lockEnd > 0 ? s.lockEnd : s.length; };
            auto find = [&](const std::string& n) -> const Section* {
                for (const auto& s : m.sections) if (s.name == n) return &s;
                return nullptr;
            };
            if (const Section* shoot = find("Shoot")) {   // bow/crossbow: Pull + Shoot
                const Section* pull = find("Pull");
                cast = (pull ? pull->length : 0.f) + t(*shoot), hits = std::max(h(*shoot), 1.f), how = "Pull+Shoot";
                return;
            }
            float ts = 0, hs = 0;
            int n = 0;
            for (const auto& s : m.sections)
                if (TwoDigitEnd(s.name)) ts += t(s), hs += h(s), ++n;
            if (n >= 2) { cast = ts / float(n), hits = std::max(hs / float(n), 1.f), how = "combo mean"; return; }
            const Section* s = find(act);
            if (!s)
                for (const auto& x : m.sections) if (!s || h(x) > h(*s)) s = &x;
            cast = s ? t(*s) : 0.f, hits = s ? std::max(h(*s), 1.f) : 1.f, how = "section";
        }

        int AbilityLevel(const Ability& a, const Params& p) {
            if (!p.learned || p.learned->empty()) return p.abilityLevel;
            for (const auto& [n, lv] : *p.learned)
                if (n == a.name || Has(a.learnedAs, n)) return lv;
            return a.name == "ShootArrow" ? 1 : 0;   // the basic attack is always there
        }
        float At(const LevelCurve& c, int lv) { return c[std::clamp(lv, 1, kAbilityLevels) - 1]; }
    }

    std::string Lower(std::string s) {
        for (char& c : s) c = char(std::tolower(static_cast<unsigned char>(c)));
        return s;
    }
    int ElementIndex(const std::string& name) {
        for (int i = 0; i < kElements; ++i) if (name == kElementNames[i]) return i;
        return -1;
    }

    dps_ctx Compiled::Ctx() const {
        return dps_ctx{A, K, int(cAb.size()), abCast.data(), abCd.data(), cAb.data(), cAp.data(), cMagic.data(), cElem.data(),
                       cCoef.data(), cHits.data(), cTargets.data(), cDotDur.data(), cDotPer.data(), scen.data()};
    }

    int Compiled::Attr(const std::string& lower) const {
        if (auto it = statAttr.find(lower); it != statAttr.end()) return it->second;
        if (lower.rfind("weapondamage_", 0) == 0)
            for (int i = 0; i < kElements; ++i)
                if (lower.substr(13) == Lower(kElementNames[i])) return -(i + 2);
        return -1;
    }

    Compiled Compile(const Tables& t, const Params& p, std::string& error) {
        Compiled c;
        auto ws = t.weaponStats.find(p.weapon);
        auto cls = t.abilities.find(p.cls);
        if (cls == t.abilities.end()) { error = "unknown class " + p.cls; return c; }
        if (ws == t.weaponStats.end()) { error = "unknown weapon type " + p.weapon; return c; }
        const std::string& wt = ws->second.animationType;
        c.cls = p.cls, c.weapon = p.weapon, c.animType = wt, c.offhand = p.offhand;
        auto dt = t.weaponDamageType.find(p.weapon);
        c.damageType = dt == t.weaponDamageType.end() ? "Slash" : dt->second;
        const Scenario sc = p.scenario ? *p.scenario : Scenario{};

        // abilities usable with this weapon and learned
        struct Comp { int k; const DamageComponent* d; };
        std::vector<Comp> comps;
        for (const Ability& a : cls->second) {
            const int lv = AbilityLevel(a, p);
            if (!lv) { c.dropped.push_back(a.name + ":not learned"); continue; }
            const auto& q = a.weaponQualifier;
            if (!q.empty() && !Has(q, wt) && !Has(q, p.offhand)) continue;
            if (a.damage.empty()) continue;
            const Montage* m = PickMontage(a, wt, p.offhand);
            if (!m) { c.dropped.push_back(a.name + ":no montage for " + wt); continue; }
            if (c.K == kMaxAbilities) { c.dropped.push_back(a.name + ":ability cap"); continue; }
            float cast = 0, hits = 1;
            std::string how;
            CastAndHits(*m, a.activateSection, a.projectile, cast, hits, how);
            float rate = At(a.animRate, lv);
            if (rate <= 0) rate = 1;
            if (auto r = a.weaponPlayRate.find(wt); r != a.weaponPlayRate.end()) rate *= r->second;
            c.abCast.push_back(cast / rate);
            c.abCd.push_back(At(a.cooldown, lv));
            c.abilities.push_back({a.name, m->name, how, lv});
            for (const auto& d : a.damage) comps.push_back({c.K, &d});
            c.cHits.resize(comps.size(), hits);   // per component below
            c.K++;
        }
        if (!c.K) { error = "no damaging ability for " + p.cls + " with " + wt; return c; }
        c.A = I_AB + 2 * c.K;
        const float nt = float(std::max(sc.targets, 1));
        for (size_t j = 0; j < comps.size(); ++j) {
            const auto& [k, d] = comps[j];
            const int lv = c.abilities[k].level;
            c.cAb.push_back(k), c.cAp.push_back(d->apRule), c.cMagic.push_back(d->magic), c.cElem.push_back(d->element);
            c.cCoef.push_back(At(d->coef, lv));
            if (d->dot) c.cHits[j] = 1.f;
            c.cTargets.push_back(d->aoe == 0 ? 1.f : d->aoe == 1 ? nt : std::min(nt, float(d->aoeCap)));
            c.cDotDur.push_back(d->dot ? At(d->dotDuration, lv) : 0.f);
            c.cDotPer.push_back(d->dot ? At(d->dotPeriod, lv) : 0.f);
        }

        // which item stat feeds which attribute
        auto& m = c.statAttr;
        m = {{"map", I_AP + 0}, {"rap", I_AP + 1}, {"sp", I_AP + 2}, {"meleepower_bonus", I_APB + 0}, {"rangedpower_bonus", I_APB + 1},
             {"spellpower_bonus", I_APB + 2}, {"weapondamage", I_WD}, {"criticalchance", I_CC}, {"basecritchance_bonus", I_BCB},
             {"criticaldamage_melee", I_CD}, {"armorpenetration", I_APEN}, {"magicpenetration", I_MPEN},
             {"increaseattackspeed", I_IAS}, {"cooldownreduction_bonus", I_CDR}};
        m[Lower("Damage_" + c.damageType)] = I_DPHYS;
        for (int i = 0; i < kElements; ++i) m[Lower(std::string("Damage_") + kElementNames[i])] = I_DEL + i;
        for (const auto& [k, d] : comps)
            if (!d->bonusStat.empty()) m[Lower(d->bonusStat)] = I_AB + k;

        // base row: AttackPowerTable (primaries + level), heroism
        c.base.assign(c.A, 0.f);
        const auto& prim = p.primary;
        for (int i = 0; i < 3; ++i) {
            auto r = t.attackPower.find(p.cls + kApRow[i]);
            if (r == t.attackPower.end()) continue;
            auto pv = [&](const std::string& s) {
                for (int j = 0; j < 6; ++j) if (s == kPrimary[j]) return prim[j];
                return 0.f;
            };
            const int L = p.level, mx = std::max(t.maxLevel, 1);
            c.base[I_AP + i] = r->second.mod1 * pv(r->second.stat1) + r->second.mod2 * pv(r->second.stat2) + float((L / mx + 1) * L) * r->second.levelMod;
        }
        if (p.heroism)
            for (const auto& [node, pts] : *p.heroism) {
                auto h = t.heroism.find(node);
                if (h == t.heroism.end()) continue;
                const float v = float(std::min(pts, h->second.maxPoints)) * h->second.perPoint;
                if (auto a = kHeroismAttr.find(node); a != kHeroismAttr.end()) c.base[a->second] += v;
                else if (auto b = kHeroismAbility.find(node); b != kHeroismAbility.end())
                    for (int k = 0; k < c.K; ++k)
                        if (c.abilities[k].name == b->second) c.base[I_AB + c.K + k] += v;
            }

        // scenario
        auto res = [&](const std::string& type) {
            for (const auto& [n, v] : sc.resist) if (n == type) return v;
            return 0.f;
        };
        auto& s = c.scen;
        s[S_L] = float(p.level), s[S_LT] = float(p.level + sc.targetLevelDelta), s[S_ARMOR] = sc.armor, s[S_MR] = sc.magicResist;
        s[S_GLANCE] = sc.glancing, s[S_DEFLECT] = sc.deflect, s[S_INC] = sc.incomingDamageMod, s[S_RESPHYS] = res(c.damageType);
        s[S_T] = sc.fightSeconds;
        for (int i = 0; i < kElements; ++i) s[S_RESEL + i] = res(kElementNames[i]);
        s[S_MODE] = sc.eventSim ? 1.f : 0.f;
        return c;
    }
}

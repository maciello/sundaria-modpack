#include "items.hpp"
#include <algorithm>
#include <cmath>
#include <random>

namespace dps::items {
    namespace {
        bool In(const std::vector<std::string>& v, const std::string& s) { return std::find(v.begin(), v.end(), s) != v.end(); }
        template<class M> const typename M::mapped_type* Find(const M& m, const typename M::key_type& k) {
            auto it = m.find(k);
            return it == m.end() ? nullptr : &it->second;
        }
        // InitAttributeSet_ScalableCraftBonus: ARMOR_EQUIVALENCY column by stat; missing row or column = 1.0
        float Equivalency(const Tables& t, const std::string& stat, const std::string& armorType, const std::string& weaponType) {
            const auto* r = Find(t.armorEquivalency, armorType.empty() ? weaponType : armorType);
            if (!r) return 1.f;
            if (stat == "MAP") return r->map;
            if (stat == "RAP") return r->rap;
            if (stat == "SP") return r->sp;
            if (stat == "ArmorFactor") return r->baseToPlate;
            if (stat == "MagicResistance") return r->resistElementalBaseToCloth;
            return 1.f;
        }
        int Round(float x) { return int(std::floor(x + 0.5f)); }   // UE Round, x >= 0
    }

    float CraftValue(const Tables& t, const std::string& stat, const std::string& slot, int level, const std::string& grade,
                     const std::string& armorType, const std::string& weaponType) {
        const auto* m = Find(t.master, stat);
        const auto* g = Find(t.gradeRows, grade);
        if (!m || !g) return 0.f;
        const auto* sd = Find(t.slotDistribution, slot == "WeaponAny" ? std::string("WeaponLeft") : slot);   // GetEquipSlotDistribution
        const float share = In(t.ignoredDistribution, stat) ? 1.f : (sd ? *sd : 0.f);
        const float pre = Equivalency(t, stat, armorType, weaponType) * share;
        float v = (*m)[std::clamp(level - 1, 0, kLevels - 1)] * pre * std::max(g->quality, 0.7f);
        if (stat.rfind("WeaponDamage_", 0) == 0) v *= g->elementalMagicMod;
        if (stat == "WeaponDamage") {
            const auto* w = Find(t.weaponStats, weaponType);
            v *= w ? w->damageModifier : 0.f;
        }
        // RoundUpAttributeValues: ceil to 1, or to 0.01 for percent stats (float32: an exact 0.01 can land on 0.02)
        const float f = In(t.percentStats, stat) ? 100.f : 1.f;
        return float(std::ceil(v * f)) / f;
    }

    Pool CraftPool(const Tables& t, const std::string& tag, const std::string& grade) {
        Pool p;
        const auto* r = Find(t.craftPools, tag);
        const auto* g = Find(t.gradeRows, grade);
        if (!r) return p;
        p.mandatory = r->mandatory;
        const auto* lists = Find(r->byGrade, grade);
        const int lo[3] = {g ? g->additionalMin : 0, g ? g->fillerMin : 0, g ? g->mandatoryByGradeMin : 0};
        const int hi[3] = {g ? g->additionalMax : 0, g ? g->fillerMax : 0, g ? g->mandatoryByGradeMax : 0};
        for (int i = 0; i < 3; ++i) {
            p.lists[i].min = lo[i], p.lists[i].max = hi[i];
            if (lists)
                for (const auto& s : (*lists)[i])
                    if (!In(p.mandatory, s)) p.lists[i].stats.push_back(s);   // mandatory stats are removed from the lists
        }
        return p;
    }

    std::map<std::string, float> SpawnChance(const Tables& t, const std::string& tag, const std::string& grade, int n) {
        const Pool p = CraftPool(t, tag, grade);
        std::mt19937 rng(1);
        std::uniform_real_distribution<float> u(0.f, 1.f);
        std::map<std::string, int> cnt;
        std::vector<std::string> got, lst;
        for (int i = 0; i < n; ++i) {
            got = p.mandatory;
            for (const PickList& l : p.lists) {
                lst = l.stats;
                const int k = l.min + Round(float(l.max - l.min) * u(rng));
                for (int j = 0; j < k && !lst.empty(); ++j) {
                    const int idx = Round(float(lst.size() - 1) * u(rng));
                    if (!In(got, lst[idx])) got.push_back(lst[idx]);   // AddUnique
                    lst.erase(lst.begin() + idx);
                }
            }
            for (const auto& s : got) cnt[s]++;
        }
        std::map<std::string, float> out;
        for (const auto& [s, c] : cnt) out[s] = float(c) / float(n);
        return out;
    }

    namespace {
        int ClassIndex(const Tables& t, const std::string& cls) {
            auto it = std::find(t.classes.begin(), t.classes.end(), cls);
            return it == t.classes.end() ? -2 : int(it - t.classes.begin());
        }
        template<class K> K MostCommon(const std::map<K, int>& c) {
            K best{};
            int n = -1;
            for (const auto& [k, v] : c) if (v > n) best = k, n = v;
            return best;
        }
        bool Scalable(const ItemSpec& s) { return s.randomStatType == 6 || s.randomStatType == 8; }
    }

    std::string ClassArmorType(const Tables& t, const std::string& cls) {
        const int job = ClassIndex(t, cls);
        std::map<std::string, int> c;
        for (const auto& [id, s] : t.specs)
            if (s.job == job && !s.armorType.empty() && Scalable(s)) c[s.armorType]++;
        return MostCommon(c);
    }

    std::map<std::string, std::string> ClassTags(const Tables& t, const std::string& cls) {
        const int job = ClassIndex(t, cls);
        std::map<std::string, std::map<std::string, int>> by;
        for (const auto& [id, s] : t.specs)
            if (s.job == job && Scalable(s) && !s.tag.empty()) by[s.equipSlot][s.tag]++;
        std::map<std::string, std::string> out;
        for (const auto& [slot, c] : by) out[slot] = MostCommon(c);
        return out;
    }
}

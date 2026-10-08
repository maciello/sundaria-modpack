#pragma once
// SDK-free player order (#16): profile weights, comparison buckets, Order. Profiles store: profiles.cpp.
#include "model.hpp"
#include <algorithm>
#include <cmath>
#include <compare>
#include <map>

namespace items {
    // Weights 0..2 (1 = neutral). focus seeds the defaults: own-attack stats 2, other-attack stats 0.
    struct Profile {
        std::string name;
        Attack focus = Attack::Unknown;
        std::map<std::string, float> weight;  // by stat name; only values the player changed
        std::map<int, float> attack;          // weapon multiplier by Attack; only changed
    };
    inline std::vector<Profile> Presets() {
        return {{"Balanced", Attack::Unknown}, {"Melee", Attack::Melee}, {"Ranged", Attack::Ranged}, {"Magic", Attack::Magic}};
    }
    inline float DefaultWeight(Attack focus, std::string_view stat) {
        const Attack a = AttackIn(stat);
        if (focus == Attack::Unknown || a == Attack::Unknown) return 1.0f;
        return a == focus ? 2.0f : 0.0f;
    }
    inline float Weight(const Profile& p, const std::string& stat) {
        auto it = p.weight.find(stat);
        return it != p.weight.end() ? it->second : DefaultWeight(p.focus, stat);
    }

    // ---- player order (ARPG convention): groups never mix; best first inside a group ----
    // Weapons, then armor, then other equipables (jewelry), then the rest (consumables, materials).
    inline int Group(const Item& it) {
        if (it.kind == Kind::Weapon) return 0;
        if (it.kind == Kind::Armor) return 1;
        return it.equipSlot >= 0 ? 2 : 3;
    }
    // Weapon attack types in profile order: own focus first, the opposite style last.
    inline int AttackRank(Attack focus, Attack a) {
        using A = Attack;
        static constexpr A order[4][3] = {{A::Melee, A::Ranged, A::Magic},   // Balanced
                                          {A::Melee, A::Magic, A::Ranged},   // Melee
                                          {A::Ranged, A::Magic, A::Melee},   // Ranged
                                          {A::Magic, A::Ranged, A::Melee}};  // Magic
        for (int i = 0; i < 3; i++) if (order[int(focus)][i] == a) return i;
        return 3;
    }
    // Items compete on stats only inside one bucket: weapons by (attack rank, weapon type), equipables by the
    // game's equipment slot (EBP_ItemEquipmentSlotEnum order), the rest as one bucket.
    struct Bucket {
        int group, a, b;
        auto operator<=>(const Bucket&) const = default;
    };
    inline Bucket BucketOf(const Item& it, Attack focus) {
        const int g = Group(it);
        if (g == 0) return {0, AttackRank(focus, it.attack), it.weaponType};
        return {g, g == 3 ? 0 : it.equipSlot, 0};
    }
    // Weapons weigh their own attack stats (a bow's RAP, a sword's Map); everything else by the profile's focus.
    inline float WeightFor(const Profile& p, const Item& it, const std::string& stat) {
        auto w = p.weight.find(stat);
        return w != p.weight.end() ? w->second : DefaultWeight(it.kind == Kind::Weapon ? it.attack : p.focus, stat);
    }
    // Indexes of items in player order: bucket, then level desc, grade desc, then score desc; spec id, slot asc.
    // score[i] = Σ weight × value / (max of that stat in the item's bucket): flat stats (RAP 83) and percentages (0.03) count alike.
    // ponytail: a stat only one item in the bucket has counts in full for it; rank-based scoring if that misorders.
    inline std::vector<int> Order(const std::vector<Item>& items, const Profile& p, const std::vector<std::string>& statNames,
                                  std::vector<float>* scoreOut = nullptr) {
        std::vector<Bucket> bucket;
        std::map<std::pair<Bucket, int>, float> maxOf;
        for (const Item& it : items) {
            bucket.push_back(BucketOf(it, p.focus));
            for (const Stat& st : it.stats) {
                float& m = maxOf[{bucket.back(), st.type}];
                m = std::max(m, std::abs(st.value));
            }
        }
        std::vector<float> score(items.size());
        for (size_t i = 0; i < items.size(); i++)
            for (const Stat& st : items[i].stats)
                if (const float m = maxOf[{bucket[i], st.type}]; m > 0)
                    score[i] += WeightFor(p, items[i], StatName(statNames, st.type)) * st.value / m;
        std::vector<int> idx(items.size());
        for (int i = 0; i < int(idx.size()); i++) idx[i] = i;
        std::stable_sort(idx.begin(), idx.end(), [&](int x, int y) {
            const Item &a = items[x], &b = items[y];
            if (bucket[x] != bucket[y]) return bucket[x] < bucket[y];
            if (a.level != b.level) return a.level > b.level;
            if (a.grade != b.grade) return a.grade > b.grade;
            if (score[x] != score[y]) return score[x] > score[y];
            if (a.specId != b.specId) return a.specId < b.specId;
            return a.slot < b.slot;
        });
        if (scoreOut) *scoreOut = std::move(score);
        return idx;
    }
}

// Profiles shared by item features (profiles.cpp); any thread. item-sort persists them in dos-tool.ini.
namespace items::profiles {
    std::vector<Profile> All();
    void SetAll(std::vector<Profile> all);
    int ActiveIndex();
    void SetActive(int i);  // clamped
    Profile Active();
}

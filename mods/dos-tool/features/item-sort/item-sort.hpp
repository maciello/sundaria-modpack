#pragma once
// SDK-free item order/filter + vanilla-sort hook logic (#21). Item model: core/items.hpp.
#include <algorithm>
#include <map>
#include <string>
#include <vector>
#include "items.hpp"

namespace item_sort {
    using namespace items;
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
    struct Filter {
        bool where[4] = {true, true, true, true};
        int kind = -1;     // -1 all, else Kind
        int attack = -1;   // -1 all, else Attack (weapons)
        std::string text;  // name contains, case-insensitive
    };
    inline bool Passes(const Item& it, const Filter& f) {
        if (!f.where[int(it.where)]) return false;
        if (f.kind >= 0 && int(it.kind) != f.kind) return false;
        if (f.attack >= 0 && int(it.attack) != f.attack) return false;
        return f.text.empty() || Has(it.name, Lower(f.text));
    }

    // ---- the game's own Sort, hooked (game thread) ----
    // UFunctions that start a vanilla sort when they pass ProcessEvent. In game only the bag header's
    // BndEvt__Button_Sort_* was seen; SortItem/RequestSortItems run inside the BP VM.
    inline bool SortTrigger(std::string_view fn) {
        return fn.starts_with("BndEvt__Button_Sort") || fn == "SortItem" || fn == "RequestSortItems";
    }
    // Byte offset of the IsStorage bool in the trigger's params, -1 when it has none (the button event).
    inline int StorageParamOffset(std::string_view fn) {
        if (fn == "SortItem") return 0x0;
        if (fn == "RequestSortItems") return 0x1;
        return -1;
    }
    inline constexpr unsigned kSettleMs = 150;  // run after the vanilla sort's burst of calls

    // Positions where the container's order after applying equals the intended order (item keys).
    inline int InOrder(const std::vector<long long>& intended, const std::vector<long long>& actual) {
        int n = 0;
        for (size_t i = 0; i < intended.size() && i < actual.size(); i++) n += intended[i] == actual[i];
        return n;
    }
}

// Game-thread API for the inventory UI (implemented in item-sort.cpp; any thread may call).
namespace item_sort::api {
    std::vector<std::string> ProfileNames();
    int ActiveProfile();
    void SetActiveProfile(int i);   // persisted in dos-tool.ini
    void RequestSort(bool bank);    // runs on the game thread at the next ProcessEvent
    std::string LastStatus();       // "inventory: … applied …" or why not
}

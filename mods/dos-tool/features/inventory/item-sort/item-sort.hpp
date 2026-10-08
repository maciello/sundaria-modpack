#pragma once
// SDK-free item order/filter + vanilla-sort hook logic (#21). Item model: ../shared/items.hpp.
#include <algorithm>
#include <map>
#include <string>
#include <vector>
#include "../shared/items.hpp"

namespace item_sort {
    using namespace items;
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
    // Empty slots below the last item (sold/salvaged items leave them). now: occupied slots ascending, from 0.
    inline int Holes(const std::vector<int>& now) { return now.empty() ? 0 : now.back() + 1 - int(now.size()); }

    // ReorderItems(SlotsToMove) puts the listed items first, in list order, keeps the rest in their order and
    // packs slots 0..n-1 (verified in game). So only the prefix up to the last position that changes needs
    // sending; 0 = in order and packed, holes alone need 1 (#75).
    // intended / now: item slots in the wanted order and in the current order.
    inline size_t PrefixToMove(const std::vector<int>& intended, const std::vector<int>& now) {
        size_t n = 0;
        for (size_t i = 0; i < intended.size(); i++)
            if (i >= now.size() || intended[i] != now[i]) n = i + 1;
        return n || intended.empty() || !Holes(now) ? n : 1;
    }

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

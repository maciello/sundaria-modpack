#pragma once
#include <cstdint>
#include <string>
#include <vector>

// Item sell (#23): items worth selling or salvaging, judged across inventory and bank.
namespace item_sell::api {
    enum class Action : std::uint8_t { Sell, Salvage };
    struct Suggestion {
        bool bank = false;               // false = inventory
        std::uint8_t containerType = 0;  // EItemContainerType raw value
        int slot = 0;                    // item slot inside that container (not the UI's compressed slot)
        Action action = Action::Sell;
        std::string reason;              // shown in the item details, e.g. "worse than <item>"
    };
    // Current suggestions. Called from the game thread (ProcessEvent listener), every 500 ms while a bag is open.
    std::vector<Suggestion> Suggested();
}

// ---- SDK-free logic (#23); item model: ../shared/items.hpp ----
#include "../shared/items.hpp"

namespace item_sell {
    using namespace items;

    // Items compete only with their own kind of slot: weapons of the same weapon type, armor/jewelry of the same
    // equipment slot. Consumables and materials never.
    inline bool SameSlot(const Item& a, const Item& b) {
        const int g = Group(a);
        if (g != Group(b) || g == 3) return false;
        return g == 0 ? a.typeName == b.typeName : a.equipSlot == b.equipSlot;
    }
    inline float StatOf(const Item& it, int type) {
        for (const Stat& s : it.stats) if (s.type == type) return s.value;
        return 0;
    }
    // a is at least as good as b: same slot, level and grade not lower, and every stat the profile values on b
    // (weight > 0) at least as high on a.
    inline bool AtLeast(const Item& a, const Item& b, const Profile& p, const std::vector<std::string>& names) {
        if (!SameSlot(a, b) || a.level < b.level || a.grade < b.grade) return false;
        for (const Stat& s : b.stats)
            if (WeightFor(p, b, StatName(names, s.type)) > 0 && StatOf(a, s.type) < s.value) return false;
        return true;
    }

    struct Pick { int item, by; };  // owned[item] is worth selling: owned[by] is at least as good
    // owned = inventory + bank + equipped. Never an equipped item, never one of the best `keep` of its slot (all owned
    // items ranked by level, grade, profile score), only items another owned item is at least as good as.
    inline std::vector<Pick> Suggest(const std::vector<Item>& owned, const Profile& p, const std::vector<std::string>& names, int keep = 1) {
        std::vector<Pick> out;
        std::vector<bool> done(owned.size());
        for (size_t i = 0; i < owned.size(); i++) {
            if (done[i] || Group(owned[i]) == 3) continue;
            std::vector<int> slot;  // indexes of every owned item of i's slot
            std::vector<Item> sub;
            for (size_t j = 0; j < owned.size(); j++)
                if (j == i || SameSlot(owned[i], owned[j])) { slot.push_back(int(j)); sub.push_back(owned[j]); done[j] = true; }
            std::vector<int> rank;  // slot positions, best first
            for (int k : Order(sub, p, names)) rank.push_back(slot[k]);
            for (size_t r = size_t(keep); r < rank.size(); r++) {
                const Item& it = owned[rank[r]];
                if (it.where == Where::Equipped) continue;
                for (size_t q = 0; q < rank.size(); q++)
                    if (q != r && AtLeast(owned[rank[q]], it, p, names)) { out.push_back({rank[r], rank[q]}); break; }
            }
        }
        return out;
    }
}

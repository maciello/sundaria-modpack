#pragma once
// The game's item slots on screen (bags of inventory, bank, vendor) and its item details panels, as the game's own
// events report them; nothing is searched for. Game thread only (inside a game::On / OnClass / OnWorldTick callback).
// Handles are SDK pointers as void*. Facts: references/game-ui.md § Item slot. Users: item-sell, item-upgrade.
#include "game.hpp"
#include "ref.hpp"
#include <cstdint>
#include <vector>

namespace items::tiles {
    // Subscribes cb to every call on a bag or bag screen, the details panel's Tick, the equipped-set change, and the world tick.
    void Listen(game::EventListener cb, bool on);

    enum class Ev : std::uint8_t { Other, Bag, Detail, Equip };
    // What a listener event is about. Bag: *what = the bag (UWidgetItemBag_C). Detail: *what = the details panel.
    // Equip: the equipped set or the held weapon set changed (equip, unequip, set switch), *what = the character (the controller for a client's set switch). references/game-events.md § Equipped set changed.
    Ev Classify(void* obj, void* fn, void** what);

    bool Note(std::vector<ref::Ref>& seen, void* w);  // adds w once; true = new
    void Prune(std::vector<ref::Ref>& seen);          // drops collected ones

    // Item slot widgets (UWidgetItemIconContainer_C) of the bags, live only. O(slots on the bags).
    std::vector<void*> Slots(const std::vector<ref::Ref>& bags);
    struct Pos { bool bank = false; std::uint8_t containerType = 0; int slot = 0; };  // = items::Item bank/containerType/slot
    bool Locate(void* slot, Pos& out);  // the game's ConvertCompressedItemSlot
    struct Shown {  // FSItemUIData identity: what a slot or a details panel shows
        int spec = 0, change = 0, icon = 0;
        bool operator==(const Shown&) const = default;
    };
    Shown OfSlot(void* slot);
    Shown OfDetail(void* detail);
}

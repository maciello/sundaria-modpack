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

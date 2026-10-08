#pragma once

// Suggested sell/salvage marks on the game's own item slots (inventory, bank, vendor) plus the reason line in the
// game's item details. Spec: references/design-system.md § Suggested sell/salvage.
namespace item_sell::badges {
    void Frame();  // render thread, every frame while the feature is on: starts the game-thread listener once
    void Off();
}

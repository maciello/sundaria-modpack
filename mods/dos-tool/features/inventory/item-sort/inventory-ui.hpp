#pragma once
// Item-sort controls inside the game's inventory/bank (inventory-ui.cpp). Called by the Item sort feature.
namespace item_sort::ui {
    void Frame();  // render thread, every frame while Item sort is on: starts the game-thread listener once
    void Off();    // render thread: stops it
}

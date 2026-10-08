#pragma once
// Puts scene quads into the game's minimap as plain UImages (solid tint) in CanvasPanel_DynamicMinimp.
// World tick only (umg::IsWorldTick): adds and removes widgets.
#include <vector>
#include "scene.hpp"

namespace dungeon_map::draw {
    // One UImage per quad, reused across ticks; setters run only for changed values. O(quads).
    void Sync(void* minimap, const std::vector<Quad>& qs);
    // Removes ours from the minimap.
    void Clear();
}

#pragma once
// Dev probe of the dungeon map's inputs (probe.cpp): dungeon → floors → slices → crumbs, doors/levers, minimap.
// Game thread only (UFunction calls).
#include <string>

namespace dungeon_map::probe {
    // YAML report; minimap = the live WidgetMiniMap_C or null.
    std::string Report(void* minimap);
    void Warm();  // the watched events' FNames
    // One log line for a watched game event (slice discovered, door/lever state change, floor activated), else "".
    std::string Event(void* obj, void* fn);
}

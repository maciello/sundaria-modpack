#pragma once

// Dev probe behind a file trigger: with dos-tool-mapsurvey.txt next to the game exe, the hub's world map scene is
// logged once - the BP_WorldMap actor with its components (class, mesh, relative transform) and every actor within
// 8000 of it (class, name, location, scale, mobility). Read on the game thread. Question: can the map scene be scaled
// down and moved onto a desk in the tavern as one unit? The file is renamed to .done afterwards.
namespace map_probe {
    void Tick();  // render thread: checks the file, the game thread surveys
}

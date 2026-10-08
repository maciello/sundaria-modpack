#pragma once
#include <string>
#include <vector>

// The hub's world map diorama (≈ 50 × 40 m on a plateau, ~35 actors: map mesh, castle, trees, the dungeon click
// zones Button_Map_*) shrunk onto a desk: every scene actor moved/scaled together (mini_map_math.hpp); lights,
// sound and cull volumes stay. Game thread work, queued from the render thread; Restore() puts it all back.
namespace mini_map {
    struct Desk { float x, y, z, yaw, width; };          // width of the miniature in cm
    void Place(const Desk& d);                            // again = move it
    void Restore();
    struct Target { std::string name; float x, y, z; };  // the map's click zones where they are now
    std::vector<Target> Targets();                        // render thread, copy
    void Stop();                                          // listener off (once, when the hub feature ends)
}

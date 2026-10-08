#pragma once
#include <string>
#include <vector>

// Furniture you can move in the hub (build mode): visible actors whose root is a static mesh that fits in a room
// (bounds radius ≤ 400), with or without collision shapes; hidden ones (our collision proxies) and `skip` excluded. Found by memory reads; highlighted with the hub's own rim outline (custom depth
// stencil = what the hub's click zones use); moved on the game thread, collision off while carried.
namespace props {
    // x,y,z: pivot (where Move puts it); cz: height of its middle (where you aim); base: pivot above the mesh's bottom
    // mesh: its full object path (duplicates load it), scale: uniform scale (x)
    struct Prop { std::string name; float x, y, z, cz, yaw, radius, base; std::string mesh; float scale; };
    void Scan(float x, float y, float z, float radius, const std::vector<std::string>& skip = {});  // asks the game thread for the props around (x,y,z), skipping these names
    std::vector<Prop> Near();                             // the last Scan's answer
    void Highlight(const std::string& name, bool on, int stencil);
    void Move(const std::string& name, float x, float y, float z, float yaw, bool carrying);
    // Back where the map had it: the spot before our first move of it in this map (a hub load applies the saved
    // layout first, so that is the game's own spot).
    void Reset(const std::string& name);
    void Stop();                                                           // listener off, once
}

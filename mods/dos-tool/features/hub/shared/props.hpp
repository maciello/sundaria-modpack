#pragma once
#include <string>
#include <vector>

// Furniture you can move in the hub (build mode): StaticMeshActors whose mesh fits in a room (bounds radius ≤ 250),
// with or without collision shapes. Found by memory reads; highlighted with the hub's own rim outline (custom depth
// stencil = what the hub's click zones use); moved on the game thread, collision off while carried.
namespace props {
    // x,y,z: pivot (where Move puts it); cz: height of its middle (where you aim); base: pivot above the mesh's bottom
    struct Prop { std::string name; float x, y, z, cz, yaw, radius, base; };
    std::vector<Prop> Near(float x, float y, float z, float radius);       // render thread
    void Highlight(const std::string& name, bool on, int stencil);
    void Move(const std::string& name, float x, float y, float z, float yaw, bool carrying);
    void Stop();                                                           // listener off, once
}

#pragma once
#include <string>
#include <vector>

// New props in the hub (build mode): copies of a mesh spawned as movable StaticMeshActors - a duplicate of a prop,
// or an item from the catalog (every furniture-sized mesh the loaded map uses). Each spawn has a stable id (the
// saved spawn list's key; actor names change per load). Game thread work, queued from the render thread.
namespace props_spawn {
    struct Item { std::string mesh, label; };  // mesh: full object path; label: mesh name
    std::vector<Item> Catalog();               // render thread: built once per map (walks the levels' actors once)
    void Spawn(int id, const std::string& mesh, float x, float y, float z, float yaw, float scale);
    void Remove(int id);
    std::string NameOf(int id);                // the spawned actor's name ("" until spawned) - render thread
    int IdOf(const std::string& actorName);    // -1 = not one of ours
    void Stop();                               // listener off, once
}

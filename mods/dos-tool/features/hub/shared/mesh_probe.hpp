#pragma once

// Dev probe behind a file trigger: while dos-tool-meshes.txt (one search term per line) sits next to the game exe,
// every level load logs the loaded StaticMeshes whose name contains a term - full path + collision (simple
// shapes, trace flag) - each mesh once per session. Read on the game thread. Question it answers:
// does some level load a colliding twin of a hub building (e.g. the inn without _HUB)?
namespace mesh_probe {
    void Tick();  // render thread: cheap until the file exists and the world changes; the game thread reports
}

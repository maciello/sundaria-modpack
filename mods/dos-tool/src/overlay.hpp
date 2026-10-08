#pragma once

namespace overlay {
    // Installs the D3D11 present/resize hooks via kiero. Returns true if the
    // render API was found and the hooks were bound.
    bool Init();
    // Unhooks and tears ImGui down so the dll can be unloaded (hot reload).
    void Shutdown();
}

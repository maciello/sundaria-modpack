#pragma once

namespace overlay {
    // Installs the D3D11 present/resize hooks via kiero. Returns true if the
    // render API was found and the hooks were bound.
    bool Init();
    // Unhooks and tears ImGui down so the dll can be unloaded (hot reload).
    void Shutdown();
    // Dev: called on the render thread in Present, after our overlay is drawn, with the IDXGISwapChain*.
    // nullptr = none (the default; one atomic load per frame).
    using PresentTap = void (*)(void* swapchain);
    void SetPresentTap(PresentTap tap);
}

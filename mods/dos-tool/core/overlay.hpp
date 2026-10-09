#pragma once
#include <atomic>

namespace feature { struct Feature; }

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
    // Dev (live bridge), any thread but the render thread: switch a feature on/off through the menu checkbox's path,
    // applied at the start of the next frame (Off() runs in that frame). Blocks until applied; false = no frame
    // within timeoutMs or `stop` set (request withdrawn). One caller at a time.
    bool RequestEnabled(feature::Feature* f, bool on, unsigned timeoutMs, const std::atomic<bool>& stop);
}

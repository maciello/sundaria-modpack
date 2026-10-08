#include "overlay.hpp"
#include "game.hpp"
#include "logger.hpp"

#include <Windows.h>
#include <d3d11.h>
#include <dxgi.h>
#include <cstdio>

#include "kiero.h"
#include "imgui.h"
#include "backends/imgui_impl_win32.h"
#include "backends/imgui_impl_dx11.h"

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND, UINT, WPARAM, LPARAM);

// kiero D3D11 table: swapchain methods occupy slots 0..17.
static constexpr uint16_t kIdxPresent = 8;  // IDXGISwapChain::Present
static constexpr uint16_t kIdxResize  = 13; // IDXGISwapChain::ResizeBuffers

typedef HRESULT(WINAPI* Present_t)(IDXGISwapChain*, UINT, UINT);
typedef HRESULT(WINAPI* Resize_t)(IDXGISwapChain*, UINT, UINT, UINT, DXGI_FORMAT, UINT);
static Present_t oPresent = nullptr;
static Resize_t  oResize  = nullptr;

static ID3D11Device*           g_device = nullptr;
static ID3D11DeviceContext*    g_context = nullptr;
static ID3D11RenderTargetView* g_rtv = nullptr;
static HWND                    g_hwnd = nullptr;
static WNDPROC                 g_oWndProc = nullptr;
static bool                    g_imguiReady = false;
static bool                    g_showMenu = true;

// ---- feature state ----------------------------------------------------------
static bool  g_seeded = false;
static bool  g_ovrFov = false;   static float g_fov = 90.0f;
static bool  g_ovrDist = false;  static float g_dist = 650.0f;
static bool  g_noCamCollision = false;
static bool  g_dmgNumbers = true;
static dmgnum::Tracker g_dmg;

static LRESULT WINAPI hkWndProc(HWND h, UINT msg, WPARAM w, LPARAM l) {
    if (msg == WM_KEYDOWN && w == VK_INSERT)
        g_showMenu = !g_showMenu;
    if (g_showMenu) {
        ImGui_ImplWin32_WndProcHandler(h, msg, w, l);
        const ImGuiIO& io = ImGui::GetIO();
        if (io.WantCaptureMouse || io.WantCaptureKeyboard)
            return true;
    }
    return CallWindowProc(g_oWndProc, h, msg, w, l);
}

static bool CreateRTV(IDXGISwapChain* sc) {
    ID3D11Texture2D* backbuffer = nullptr;
    if (FAILED(sc->GetBuffer(0, __uuidof(ID3D11Texture2D), (void**)&backbuffer)) || !backbuffer)
        return false;
    HRESULT hr = g_device->CreateRenderTargetView(backbuffer, nullptr, &g_rtv);
    backbuffer->Release();
    return SUCCEEDED(hr);
}
static void ReleaseRTV() { if (g_rtv) { g_rtv->Release(); g_rtv = nullptr; } }

static bool InitImGui(IDXGISwapChain* sc) {
    if (FAILED(sc->GetDevice(__uuidof(ID3D11Device), (void**)&g_device)))
        return false;
    g_device->GetImmediateContext(&g_context);

    DXGI_SWAP_CHAIN_DESC desc{};
    sc->GetDesc(&desc);
    g_hwnd = desc.OutputWindow;
    if (!CreateRTV(sc))
        return false;

    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.LogFilename = nullptr;
    ImGui::StyleColorsDark();
    ImGui_ImplWin32_Init(g_hwnd);
    ImGui_ImplDX11_Init(g_device, g_context);
    g_oWndProc = (WNDPROC)SetWindowLongPtr(g_hwnd, GWLP_WNDPROC, (LONG_PTR)hkWndProc);
    logger::log("[overlay] ImGui (D3D11) initialised");
    return true;
}

static void DrawMenu(const game::Snapshot& snap) {
    ImGui::GetIO().MouseDrawCursor = g_showMenu;
    if (!g_showMenu) return;

    ImGui::SetNextWindowSize(ImVec2(340, 210), ImGuiCond_FirstUseEver);
    ImGui::Begin("Dungeons of Sundaria");

    ImGui::Text("FPS %.0f", ImGui::GetIO().Framerate);
    ImGui::SameLine();
    ImGui::TextDisabled("UE 4.27.2 | GObjects %d", snap.objectCount);
    if (snap.valid)
        ImGui::TextDisabled("World: %s", snap.worldName.c_str());
    else
        ImGui::TextColored(ImVec4(1, 0.5f, 0.2f, 1), "Waiting for UWorld...");

    ImGui::SeparatorText("Camera");
    ImGui::Checkbox("Override FOV", &g_ovrFov);
    ImGui::SameLine(); ImGui::SetNextItemWidth(150);
    ImGui::SliderFloat("##fov", &g_fov, 40.0f, 130.0f, "%.0f");
    ImGui::Checkbox("Override distance", &g_ovrDist);
    ImGui::SameLine(); ImGui::SetNextItemWidth(150);
    ImGui::SliderFloat("##dist", &g_dist, 150.0f, 2200.0f, "%.0f");
    ImGui::Checkbox("Disable camera collision (no zoom-in at walls)", &g_noCamCollision);

    ImGui::SeparatorText("Combat");
    ImGui::Checkbox("Damage numbers", &g_dmgNumbers);

    ImGui::Separator();
    ImGui::TextDisabled("[INSERT] toggle menu");
    ImGui::End();
}

static void DrawDamageNumbers() {
    const double now = ImGui::GetTime();
    g_dmg.Update(game::SampleHealth(), now);
    dmgnum::View view;
    if (g_dmg.live.empty() || !game::GetView(view)) return;
    const ImVec2 screen = ImGui::GetIO().DisplaySize;
    ImDrawList* dl = ImGui::GetForegroundDrawList();
    ImFont* font = ImGui::GetFont();
    for (const dmgnum::Number& n : g_dmg.live) {
        float sx, sy;
        if (!dmgnum::Project(view, n.x, n.y, n.z, screen.x, screen.y, sx, sy)) continue;
        const float t = float((now - n.born) / g_dmg.lifetime);   // 0..1
        const int alpha = int(255 * (1.0f - t));
        const ImU32 col = n.amount > 0 ? IM_COL32(255, 220, 60, alpha) : IM_COL32(80, 255, 120, alpha);
        char buf[16];
        std::snprintf(buf, sizeof(buf), "%.0f", n.amount > 0 ? n.amount : -n.amount);
        const ImVec2 pos(sx - 10.0f, sy - 40.0f * t);              // rise while fading
        dl->AddText(font, 26.0f, ImVec2(pos.x + 2, pos.y + 2), IM_COL32(0, 0, 0, alpha), buf);
        dl->AddText(font, 26.0f, pos, col, buf);
    }
}

static void ApplyFeatures() {
    if (g_ovrFov)  game::SetFOV(g_fov);
    if (g_ovrDist) game::SetCameraDistance(g_dist);
    game::SetCameraCollision(!g_noCamCollision);
}

static HRESULT WINAPI hkPresent(IDXGISwapChain* sc, UINT syncInterval, UINT flags) {
    if (!g_imguiReady) {
        if (!InitImGui(sc))
            return oPresent(sc, syncInterval, flags);
        g_imguiReady = true;
    }
    if (!g_rtv)
        return oPresent(sc, syncInterval, flags);

    // Refresh live readout a few dozen times/sec.
    static game::Snapshot snap;
    static ULONGLONG last = 0;
    const ULONGLONG now = GetTickCount64();
    if (now - last >= 100) { snap = game::Gather(); last = now; }

    // Seed sliders from the game's real values the first time we see them.
    if (!g_seeded && snap.haveCamera) {
        g_fov = game::OriginalFOV();
        g_dist = game::OriginalDistance();
        g_seeded = true;
    }

    ImGui_ImplDX11_NewFrame();
    ImGui_ImplWin32_NewFrame();
    ImGui::NewFrame();

    ApplyFeatures();
    if (g_dmgNumbers) DrawDamageNumbers();
    DrawMenu(snap);

    ImGui::Render();
    g_context->OMSetRenderTargets(1, &g_rtv, nullptr);
    ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
    return oPresent(sc, syncInterval, flags);
}

static HRESULT WINAPI hkResize(IDXGISwapChain* sc, UINT bufferCount, UINT w, UINT h,
                               DXGI_FORMAT fmt, UINT flags) {
    if (g_imguiReady) ReleaseRTV();
    HRESULT hr = oResize(sc, bufferCount, w, h, fmt, flags);
    if (g_imguiReady) CreateRTV(sc);
    return hr;
}

bool overlay::Init() {
    if (kiero::init(kiero::RenderType::D3D11) != kiero::Status::Success) {
        logger::log("[overlay] kiero::init(D3D11) failed");
        return false;
    }
    kiero::bind(kIdxPresent, (void**)&oPresent, (void*)hkPresent);
    kiero::bind(kIdxResize,  (void**)&oResize,  (void*)hkResize);
    logger::log("[overlay] kiero D3D11 hooks bound (Present/Resize)");
    return true;
}

void overlay::Shutdown() {
    kiero::shutdown();  // restores Present/ResizeBuffers
    Sleep(200);         // let an in-flight hkPresent finish before tearing down
    if (g_oWndProc) { SetWindowLongPtr(g_hwnd, GWLP_WNDPROC, (LONG_PTR)g_oWndProc); g_oWndProc = nullptr; }
    if (g_imguiReady) {
        ImGui_ImplDX11_Shutdown();
        ImGui_ImplWin32_Shutdown();
        ImGui::DestroyContext();
        g_imguiReady = false;
    }
    ReleaseRTV();
    if (g_context) { g_context->Release(); g_context = nullptr; }
    if (g_device) { g_device->Release(); g_device = nullptr; }
}

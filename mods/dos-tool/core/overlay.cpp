#include "overlay.hpp"
#include "game.hpp"
#include "logger.hpp"
#include "feature.hpp"

#include <Windows.h>
#include <d3d11.h>
#include <dxgi.h>
#include <cstdio>
#include <cstring>
#include <algorithm>
#include <cfloat>

#include "kiero.h"
#include "fonts.h"
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
static bool                    g_showMenu = false;
static ImFont*                 g_fonts[sizeof(kFonts) / sizeof(kFonts[0])] = {};
static int                     g_font = 1;  // Titan One

static combat::Tracker g_combat;

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
    io.Fonts->AddFontDefault();
    ImFontConfig cfg;
    cfg.FontDataOwnedByAtlas = false;
    for (int i = 0; i < IM_ARRAYSIZE(kFonts); i++)
        g_fonts[i] = io.Fonts->AddFontFromMemoryTTF((void*)kFonts[i].data, kFonts[i].size, 64.0f, &cfg);
    ImGui_ImplWin32_Init(g_hwnd);
    ImGui_ImplDX11_Init(g_device, g_context);
    g_oWndProc = (WNDPROC)SetWindowLongPtr(g_hwnd, GWLP_WNDPROC, (LONG_PTR)hkWndProc);
    logger::log("[overlay] ImGui (D3D11) initialised");
    return true;
}

static void DrawMenu(const game::Snapshot& snap) {
    ImGui::GetIO().MouseDrawCursor = g_showMenu;
    if (!g_showMenu) return;

    ImGui::SetNextWindowSize(ImVec2(360, 420), ImGuiCond_FirstUseEver);
    ImGui::Begin("Dungeons of Sundaria");

    ImGui::Text("FPS %.0f", ImGui::GetIO().Framerate);
    ImGui::SameLine();
    ImGui::TextDisabled("UE 4.27.2 | GObjects %d", snap.objectCount);
    if (snap.valid)
        ImGui::TextDisabled("World: %s", snap.worldName.c_str());
    else
        ImGui::TextColored(ImVec4(1, 0.5f, 0.2f, 1), "Waiting for UWorld...");
    if (ImGui::BeginCombo("Font", kFonts[g_font].name)) {
        for (int i = 0; i < IM_ARRAYSIZE(kFonts); i++)
            if (ImGui::Selectable(kFonts[i].name, i == g_font)) g_font = i;
        ImGui::EndCombo();
    }

    for (feature::Feature* f : feature::Feature::All()) {
        ImGui::SeparatorText(f->name);
        ImGui::PushID(f->name);
        ImGui::Checkbox("Enabled", &f->enabled);
        if (f->enabled) f->Menu();
        ImGui::PopID();
    }

    ImGui::Separator();
    ImGui::TextDisabled("[INSERT] toggle menu");
    ImGui::End();
}

static void RunFeatures(const game::Snapshot& snap) {
    const ImVec2 screen = ImGui::GetIO().DisplaySize;
    const double now = ImGui::GetTime();
    const std::vector<combat::Sample> chars = game::SampleHealth();
    g_combat.Update(chars, now);
    const feature::Frame fr{now, screen.x, screen.y, g_fonts[g_font] ? g_fonts[g_font] : ImGui::GetFont(), snap, g_combat, chars};
    for (feature::Feature* f : feature::Feature::All()) {
        if (f->wasEnabled && !f->enabled) f->Off();
        f->wasEnabled = f->enabled;
        if (f->enabled) f->OnFrame(fr);
    }
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

    ImGui_ImplDX11_NewFrame();
    ImGui_ImplWin32_NewFrame();
    ImGui::NewFrame();

    RunFeatures(snap);
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
    for (feature::Feature* f : feature::Feature::All()) if (f->enabled) f->Off();  // unload = vanilla
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

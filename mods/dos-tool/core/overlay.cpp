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
#include <atomic>
#include <vector>

#include "kiero.h"
#include "fonts.h"
#include "imgui.h"
#include "imgui_internal.h"  // ImGuiSettingsHandler
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
static bool                    g_dev = false;      // dos-tool.dev next to the exe: show Alpha features
static char                    g_ini[MAX_PATH] = {};

static combat::Tracker g_combat;
static std::atomic<overlay::PresentTap> g_tap{nullptr};
void overlay::SetPresentTap(PresentTap tap) { g_tap = tap; }

static bool InputCaptured() {
    for (const feature::Feature* f : feature::Feature::All())
        if (f->enabled && f->CapturesInput()) return true;
    return false;
}

static bool KeyPassed(unsigned vk) {
    for (const feature::Feature* f : feature::Feature::All())
        if (f->enabled && f->CapturesInput() && f->PassesKey(vk)) return true;
    return false;
}

// Raw mouse motion while input is captured: the hidden cursor can be frozen by the game, raw deltas can't.
// Registered on the window's (game) thread only if the game hasn't registered the mouse itself; removed after.
static std::atomic<long> g_rawDX{0}, g_rawDY{0};
static std::atomic<bool> g_rawSeen{false};
static bool g_rawOurs = false;

static void UpdateRawMouse(bool want) {
    if (want == g_rawOurs) return;
    if (want) {
        UINT n = 0;
        GetRegisteredRawInputDevices(nullptr, &n, sizeof(RAWINPUTDEVICE));
        std::vector<RAWINPUTDEVICE> regs(n);
        if (n && GetRegisteredRawInputDevices(regs.data(), &n, sizeof(RAWINPUTDEVICE)) == UINT(-1)) n = 0;
        for (UINT i = 0; i < n; i++)
            if (regs[i].usUsagePage == 1 && regs[i].usUsage == 2) return;  // the game already gets raw mouse: just listen
        RAWINPUTDEVICE rid{1, 2, 0, g_hwnd};
        g_rawOurs = RegisterRawInputDevices(&rid, 1, sizeof(rid)) == TRUE;
    } else {
        RAWINPUTDEVICE rid{1, 2, RIDEV_REMOVE, nullptr};
        RegisterRawInputDevices(&rid, 1, sizeof(rid));
        g_rawOurs = false;
    }
}

static LRESULT WINAPI hkWndProc(HWND h, UINT msg, WPARAM w, LPARAM l) {
    if (msg == WM_KEYDOWN && w == VK_INSERT)
        g_showMenu = !g_showMenu;
    if (g_showMenu) {
        ImGui_ImplWin32_WndProcHandler(h, msg, w, l);
        const ImGuiIO& io = ImGui::GetIO();
        if (io.WantCaptureMouse || io.WantCaptureKeyboard)
            return true;
    }
    const bool captured = !g_showMenu && InputCaptured();
    UpdateRawMouse(captured);
    if (msg == WM_INPUT && captured) {
        RAWINPUT ri{};
        UINT size = sizeof(ri);
        if (GetRawInputData(reinterpret_cast<HRAWINPUT>(l), RID_INPUT, &ri, &size, sizeof(RAWINPUTHEADER)) != UINT(-1)
            && ri.header.dwType == RIM_TYPEMOUSE && !(ri.data.mouse.usFlags & MOUSE_MOVE_ABSOLUTE)) {
            g_rawDX += ri.data.mouse.lLastX;
            g_rawDY += ri.data.mouse.lLastY;
            g_rawSeen = true;
        }
    }
    // A release is swallowed exactly when its press was: the game never sees half a key (hub buttons fire on
    // release), and keys it saw go down (e.g. the one that started the capture) still come up for it.
    static bool swallowedKey[256] = {}, swallowedButton[3] = {};
    const int button = (msg == WM_LBUTTONDOWN || msg == WM_LBUTTONUP) ? 0 : (msg == WM_RBUTTONDOWN || msg == WM_RBUTTONUP) ? 1
                     : (msg == WM_MBUTTONDOWN || msg == WM_MBUTTONUP) ? 2 : -1;
    if ((msg == WM_KEYUP || msg == WM_SYSKEYUP) && w < 256 && swallowedKey[w]) { swallowedKey[w] = false; return 0; }
    if ((msg == WM_LBUTTONUP || msg == WM_RBUTTONUP || msg == WM_MBUTTONUP) && swallowedButton[button]) {
        swallowedButton[button] = false;
        return 0;
    }
    if (captured) {
        switch (msg) {
            case WM_KEYDOWN: case WM_SYSKEYDOWN:
                if (w == VK_INSERT || KeyPassed(unsigned(w))) break;
                if (w < 256) swallowedKey[w] = true;
                return 0;
            case WM_LBUTTONDOWN: case WM_RBUTTONDOWN: case WM_MBUTTONDOWN:
                swallowedButton[button] = true;
                return 0;
            case WM_CHAR: case WM_SYSCHAR: case WM_XBUTTONDOWN:
            case WM_LBUTTONDBLCLK: case WM_RBUTTONDBLCLK: case WM_MOUSEWHEEL: case WM_MOUSEMOVE:
                return 0;
            case WM_SETCURSOR:
                SetCursor(nullptr);
                return TRUE;
        }
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

// Stable on; with dos-tool.dev every non-Deprecated, non-optIn feature on. The ini stores only deviations from this.
// Lines naming a feature this build lacks ("X=on,stage" or "X.key=v"): written back unchanged, so swapping
// builds never drops another build's toggles (#69).
static std::vector<std::string> g_foreign;
static bool KnownName(const char* line, const char* eq) {
    const char* dot = static_cast<const char*>(memchr(line, '.', eq - line));
    const size_t n = (dot ? dot : eq) - line;
    for (feature::Feature* f : feature::Feature::All())
        if (strlen(f->name) == n && !strncmp(f->name, line, n)) return true;
    return !strncmp(line, "Font", 4) && n == 4;
}

static bool Default(const feature::Feature* f) {
    return g_dev ? f->stage != feature::Stage::Deprecated && !f->optIn : f->stage == feature::Stage::Stable;
}

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
    GetModuleFileNameA(nullptr, g_ini, MAX_PATH);
    if (char* slash = strrchr(g_ini, '\\')) slash[1] = 0;
    char devFlag[MAX_PATH];
    snprintf(devFlag, MAX_PATH, "%sdos-tool.dev", g_ini);
    g_dev = GetFileAttributesA(devFlag) != INVALID_FILE_ATTRIBUTES;
    for (feature::Feature* f : feature::Feature::All()) f->enabled = f->wasEnabled = Default(f);
    strncat(g_ini, "dos-tool.ini", MAX_PATH - strlen(g_ini) - 1);
    io.IniFilename = g_ini;  // window layout + [DosTool][Settings] below; local to this install
    ImGuiSettingsHandler h{};
    h.TypeName = "DosTool";
    h.TypeHash = ImHashStr("DosTool");
    h.ReadOpenFn = [](ImGuiContext*, ImGuiSettingsHandler*, const char*) -> void* { return (void*)1; };
    h.ReadLineFn = [](ImGuiContext*, ImGuiSettingsHandler*, void*, const char* line) {
        int v;
        if (sscanf(line, "Font=%d", &v) == 1 && v >= 0 && v < IM_ARRAYSIZE(kFonts)) { g_font = v; return; }
        for (feature::Feature* f : feature::Feature::All()) {  // "<feature name>.<key>=<value>"
            const size_t n = strlen(f->name);
            if (strncmp(line, f->name, n) || line[n] != '.') continue;
            if (const char* e = strchr(line + n + 1, '=')) f->Load(std::string(line + n + 1, e).c_str(), e + 1);
            return;
        }
        const char* eq = strrchr(line, '=');
        if (!eq) return;
        if (!KnownName(line, eq)) { g_foreign.emplace_back(line); return; }  // another build's feature: kept on save (#69)
        int on, stage;  // "<name>=<on>,<stage>": a choice made under another stage is dropped, so promotions apply
        if (sscanf(eq + 1, "%d,%d", &on, &stage) != 2) return;
        for (feature::Feature* f : feature::Feature::All())
            if (strlen(f->name) == size_t(eq - line) && !strncmp(f->name, line, eq - line) && stage == int(f->stage))
                f->enabled = f->wasEnabled = on == 1;
    };
    h.WriteAllFn = [](ImGuiContext*, ImGuiSettingsHandler* hh, ImGuiTextBuffer* out) {
        out->appendf("[%s][Settings]\nFont=%d\n", hh->TypeName, g_font);
        for (feature::Feature* f : feature::Feature::All())
            if (f->enabled != Default(f)) out->appendf("%s=%d,%d\n", f->name, f->enabled ? 1 : 0, int(f->stage));
        std::vector<std::pair<std::string, std::string>> kv;
        for (feature::Feature* f : feature::Feature::All()) {
            kv.clear();
            f->Save(kv);
            for (auto& [k, v] : kv) out->appendf("%s.%s=%s\n", f->name, k.c_str(), v.c_str());
        }
        for (const std::string& l : g_foreign) out->appendf("%s\n", l.c_str());
        out->append("\n");
    };
    ImGui::AddSettingsHandler(&h);
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

// The one on/off path (menu checkbox, live bridge): render thread. RunFeatures calls Off() on the next pass.
static void SetEnabled(feature::Feature* f, bool on) {
    f->enabled = on;
    ImGui::MarkIniSettingsDirty();
}

static std::atomic<feature::Feature*> g_req{nullptr};  // RequestEnabled -> next frame
static std::atomic<bool> g_reqOn{false}, g_reqDone{false};

bool overlay::RequestEnabled(feature::Feature* f, bool on, unsigned timeoutMs, const std::atomic<bool>& stop) {
    g_reqOn = on;
    g_reqDone = false;
    g_req = f;
    const ULONGLONG until = GetTickCount64() + timeoutMs;
    while (!g_reqDone) {
        if (stop || GetTickCount64() > until) {
            feature::Feature* posted = f;
            if (g_req.compare_exchange_strong(posted, nullptr)) return false;  // else: being applied right now
        }
        Sleep(2);
    }
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
            if (ImGui::Selectable(kFonts[i].name, i == g_font)) { g_font = i; ImGui::MarkIniSettingsDirty(); }
        ImGui::EndCombo();
    }

    static const char* const kStage[] = {"ALPHA", "BETA", "", "DEPRECATED"};
    static const ImVec4 kStageColor[] = {{1, 0.35f, 0.35f, 1}, {1, 0.8f, 0.3f, 1}, {}, {0.6f, 0.6f, 0.6f, 1}};
    // In-development features first; finished (Stable) ones folded away under one header.
    for (feature::Stage stage : {feature::Stage::Alpha, feature::Stage::Beta, feature::Stage::Stable, feature::Stage::Deprecated}) {
        const int st = int(stage);
        if (stage == feature::Stage::Alpha && !g_dev) continue;
        if (stage == feature::Stage::Stable && !ImGui::CollapsingHeader("Finished features")) continue;
        for (feature::Feature* f : feature::Feature::All()) {
            if (f->stage != stage) continue;
            ImGui::SeparatorText(f->name);
            ImGui::PushID(f->name);
            bool on = f->enabled;
            if (ImGui::Checkbox("Enabled", &on)) SetEnabled(f, on);
            if (*kStage[st]) { ImGui::SameLine(); ImGui::TextColored(kStageColor[st], "%s", kStage[st]); }
            if (f->enabled) f->Menu();
            ImGui::PopID();
        }
    }

    ImGui::Separator();
    ImGui::TextDisabled("[INSERT] toggle menu");
    ImGui::End();
}

static void RunFeatures(const game::Snapshot& snap) {
    const ImVec2 screen = ImGui::GetIO().DisplaySize;
    const double now = ImGui::GetTime();
    bool wantCombat = false;  // core work runs only for enabled features that need it
    for (const feature::Feature* f : feature::Feature::All()) wantCombat |= f->enabled && f->usesCombat;
    const std::vector<combat::Sample> chars = game::SampleHealth(wantCombat);
    g_combat.Update(chars, now);
    feature::Frame fr{now, screen.x, screen.y, g_fonts[g_font] ? g_fonts[g_font] : ImGui::GetFont(), snap, g_combat, chars};
    fr.mouseDX = float(g_rawDX.exchange(0));
    fr.mouseDY = float(g_rawDY.exchange(0));
    fr.rawMouse = g_rawSeen.load();
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

    if (feature::Feature* f = g_req.exchange(nullptr)) { SetEnabled(f, g_reqOn); g_reqDone = true; }
    RunFeatures(snap);
    DrawMenu(snap);

    ImGui::Render();
    g_context->OMSetRenderTargets(1, &g_rtv, nullptr);
    ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
    if (overlay::PresentTap tap = g_tap.load(std::memory_order_relaxed)) tap(sc);
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
    // Present/Resize first: else a frame in between re-applies what Off() restored.
    kiero::unbind(kIdxPresent);
    kiero::unbind(kIdxResize);
    Sleep(200);  // let an in-flight hkPresent finish before tearing down
    // unload = vanilla. Off() runs with the ProcessEvent hook still alive: restores and widget removals that need the
    // game thread complete on a real world tick (kiero::shutdown disables every MinHook hook, that one included) (#84).
    for (feature::Feature* f : feature::Feature::All()) if (f->enabled) f->Off();
    kiero::shutdown();  // any ProcessEvent hook still left is gone here
    Sleep(100);         // in-flight hooked calls leave our code before the DLL goes
    if (g_oWndProc) { SetWindowLongPtr(g_hwnd, GWLP_WNDPROC, (LONG_PTR)g_oWndProc); g_oWndProc = nullptr; }
    if (g_rawOurs) { RAWINPUTDEVICE rid{1, 2, RIDEV_REMOVE, nullptr}; RegisterRawInputDevices(&rid, 1, sizeof(rid)); g_rawOurs = false; }
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

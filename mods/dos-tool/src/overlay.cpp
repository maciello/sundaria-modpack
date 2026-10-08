#include "overlay.hpp"
#include "game.hpp"
#include "logger.hpp"

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

// ---- feature state ----------------------------------------------------------
static bool  g_seeded = false;
static bool  g_ovrFov = false;   static float g_fov = 90.0f;
static bool  g_ovrDist = false;  static float g_dist = 650.0f;
static bool  g_noCamCollision = false;
static bool  g_dmgNumbers = true;
static bool  g_dpsPanel = true;
static float g_height = 40.0f;  // cm above capsule center
static float g_size = 1.0f;
struct Preview { float sx, sy; dmgnum::Number n; std::vector<std::pair<double, float>> ticks; };
static std::vector<Preview> g_preview;
static float g_previewTypical = 20;
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
    ImGui::Checkbox("DPS panel", &g_dpsPanel);
    ImGui::SliderFloat("Number height", &g_height, -60.0f, 160.0f, "%.0f cm");
    ImGui::SliderFloat("Number size", &g_size, 0.5f, 2.0f, "%.2fx");
    if (ImGui::BeginCombo("Font", kFonts[g_font].name)) {
        for (int i = 0; i < IM_ARRAYSIZE(kFonts); i++)
            if (ImGui::Selectable(kFonts[i].name, i == g_font)) g_font = i;
        ImGui::EndCombo();
    }
    if (ImGui::Button("Preview numbers")) {
        const ImVec2 c(ImGui::GetIO().DisplaySize.x * 0.5f, ImGui::GetIO().DisplaySize.y * 0.45f);
        const double now = ImGui::GetTime();
        const float amounts[] = {18, 24, 21, 95, 19, 260};
        dmgnum::Tracker scratch;  // don't let fake hits move the real "typical hit"
        scratch.typical = 20;
        for (int i = 0; i < 6; i++) {
            const float sc = scratch.Scale(amounts[i]);
            g_preview.push_back({c.x + (i - 2.5f) * 50.0f, c.y, {0, 0, 0, amounts[i], dmgnum::Kind::Dealt, sc, (i % 3) - 1.0f, now + i * 0.18}});
        }
        g_preview.push_back({c.x - 220, c.y + 80, {0, 0, 0, 35, dmgnum::Kind::Taken, 0.95f, -1, now + 0.4}});
        g_preview.push_back({c.x + 220, c.y + 80, {0, 0, 0, 50, dmgnum::Kind::Heal, 0.9f, 1, now + 0.7}});
        Preview dot{c.x, c.y + 170, {0, 0, 0, 12, dmgnum::Kind::Dealt, scratch.Rel(12), 0.3f, now + 1.2}};  // DoT: 8 ticks stack
        for (int i = 1; i < 8; i++) dot.ticks.push_back({now + 1.2 + i * 0.45, 12.0f + i});
        g_preview.push_back(dot);
        g_previewTypical = scratch.typical;
    }

    ImGui::Separator();
    ImGui::TextDisabled("[INSERT] toggle menu");
    ImGui::End();
}

static void FormatAmount(char* buf, size_t n, double v) {
    if (v >= 1e6)      std::snprintf(buf, n, "%.1fM", v / 1e6);
    else if (v >= 1e4) std::snprintf(buf, n, "%.1fk", v / 1e3);
    else               std::snprintf(buf, n, "%.0f", v);
}

static void OutlinedText(ImDrawList* dl, ImFont* f, float size, ImVec2 p, ImU32 col, ImU32 outline, float w, const char* s) {
    for (int dx = -1; dx <= 1; dx++)
        for (int dy = -1; dy <= 1; dy++)
            if (dx || dy) dl->AddText(f, size, ImVec2(p.x + dx * w, p.y + dy * w), outline, s);
    dl->AddText(f, size, p, col, s);
}

static void DrawNumber(ImDrawList* dl, ImFont* font, float base, float sx, float sy, const dmgnum::Number& n, double now) {
    const double sinceBorn = now - n.born, sinceBump = now - n.bump;
    if (sinceBorn < 0) return;
    const float big = std::clamp((n.scale - 1.0f) / 0.8f, 0.0f, 1.0f);
    const dmgnum::Anim an = dmgnum::Animate(sinceBorn, sinceBump, g_dmg.lifetime, n.drift,
                                            n.kind == dmgnum::Kind::Dealt ? big : 0.0f, n.hits > 1);
    if (an.scale <= 0.01f || an.alpha <= 0.0f) return;
    char buf[24];
    FormatAmount(buf, sizeof(buf) - 1, n.amount);
    float r, g, b;
    switch (n.kind) {
        case dmgnum::Kind::Heal:  r = 110; g = 255; b = 140; std::memmove(buf + 1, buf, strlen(buf) + 1); buf[0] = '+'; break;
        case dmgnum::Kind::Taken: r = 255; g = 80;  b = 70;  break;
        default:                  r = 255; g = 255 - 70 * big; b = 255 - 200 * big;  // white -> gold
    }
    r += (255 - r) * an.flash; g += (255 - g) * an.flash; b += (255 - b) * an.flash;  // impact flash
    const int a = int(255 * an.alpha);
    const float unit = base * g_size * n.scale;
    const float size = unit * an.scale;
    const ImVec2 ts = font->CalcTextSizeA(size, FLT_MAX, 0.0f, buf);
    const ImVec2 pos(sx + an.dx * unit - ts.x * 0.5f, sy + an.dy * unit - ts.y * 0.5f);
    const float ow = std::max(1.5f, size / 16.0f);
    const ImU32 outline = IM_COL32(20, 12, 8, int(a * 0.85f));
    if (an.flash > 0)  // soft glow while hot
        OutlinedText(dl, font, size, pos, IM_COL32(0, 0, 0, 0), IM_COL32(int(r), int(g), int(b), int(90 * an.flash * an.alpha)), ow * 3.0f, buf);
    OutlinedText(dl, font, size, pos, IM_COL32(int(r), int(g), int(b), a), outline, ow, buf);
    if (n.hits > 1) {  // hit counter, sits on the top-right shoulder
        char cnt[16];
        std::snprintf(cnt, sizeof(cnt), "x%d", n.hits);
        const float cs = std::max(14.0f, unit * 0.42f);
        OutlinedText(dl, font, cs, ImVec2(pos.x + ts.x + 2, pos.y - cs * 0.15f), IM_COL32(255, 210, 120, a), outline, std::max(1.0f, cs / 14.0f), cnt);
    }
}

static void DrawDamageNumbers() {
    const double now = ImGui::GetTime();
    const ImVec2 screen = ImGui::GetIO().DisplaySize;
    const float base = screen.y / 26.0f;  // ~42 px at 1080p for a typical hit
    ImDrawList* dl = ImGui::GetForegroundDrawList();
    ImFont* font = g_fonts[g_font] ? g_fonts[g_font] : ImGui::GetFont();
    dmgnum::Tracker rel;
    rel.typical = g_previewTypical;
    for (Preview& p : g_preview)
        while (!p.ticks.empty() && p.ticks.front().first <= now) {
            p.n.amount += p.ticks.front().second;
            p.n.hits++;
            p.n.bump = p.ticks.front().first;
            p.n.scale = rel.Rel(p.n.amount);
            p.ticks.erase(p.ticks.begin());
        }
    std::erase_if(g_preview, [&](const Preview& p) { return p.ticks.empty() && now - p.n.bump > g_dmg.lifetime; });
    for (const Preview& p : g_preview) DrawNumber(dl, font, base, p.sx, p.sy, p.n, now);
    dmgnum::View view;
    if (g_dmg.live.empty() || !game::GetView(view)) return;
    for (const dmgnum::Number& n : g_dmg.live) {
        float sx, sy;
        if (dmgnum::Project(view, n.x, n.y, n.z + g_height, screen.x, screen.y, sx, sy))
            DrawNumber(dl, font, base, sx, sy, n, now);
    }
}

static void DrawDpsPanel() {
    const double now = ImGui::GetTime();
    if (!g_dmg.inFight) return;
    const dmgnum::Fight& f = g_dmg.fight;
    const bool active = g_dmg.FightActive(now);
    char dps[24], total[24];
    FormatAmount(dps, sizeof(dps), f.Dps());
    FormatAmount(total, sizeof(total), f.total);
    const ImVec2 screen = ImGui::GetIO().DisplaySize;
    ImGui::SetNextWindowPos(ImVec2(screen.x - 16, 16), ImGuiCond_Always, ImVec2(1, 0));
    ImGui::SetNextWindowBgAlpha(0.35f);
    ImGui::Begin("##dps", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoInputs |
                 ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav);
    if (g_fonts[g_font]) ImGui::PushFont(g_fonts[g_font]);
    ImGui::SetWindowFontScale(0.4f);
    ImGui::TextColored(active ? ImVec4(1, 0.85f, 0.4f, 1) : ImVec4(0.7f, 0.7f, 0.7f, 1), "DPS %s", dps);
    ImGui::SetWindowFontScale(0.28f);
    ImGui::Text("total %s  |  %.0fs", total, f.Duration());
    if (g_fonts[g_font]) ImGui::PopFont();
    ImGui::End();
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
    if (g_dmgNumbers || g_dpsPanel) g_dmg.Update(game::SampleHealth(), ImGui::GetTime());
    if (g_dmgNumbers) DrawDamageNumbers();
    if (g_dpsPanel) DrawDpsPanel();
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

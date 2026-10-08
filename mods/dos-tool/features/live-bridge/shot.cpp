#include "live-bridge.hpp"
#include "logger.hpp"
#include "overlay.hpp"

#include <Windows.h>
#include <d3d11.h>
#include <dxgi.h>
#include <atomic>
#include <cstdio>

// Live bridge `shot`: the Present tap copies the back buffer (overlay included) to a staging texture once and
// converts it to RGB; the bridge thread crops, encodes the PNG and writes the file. No tap while no shot is pending.
namespace {
    struct Frame { int w = 0, h = 0; std::vector<uint8_t> rgb; std::string err; };
    Frame g_frame;                    // written by the tap, read by the bridge thread after g_done
    std::atomic<bool> g_done{false};

    bool ToRgb(DXGI_FORMAT f, const uint8_t* src, int w, uint8_t* dst) {
        switch (f) {
            case DXGI_FORMAT_R8G8B8A8_UNORM: case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB: case DXGI_FORMAT_R8G8B8A8_TYPELESS:
                for (int x = 0; x < w; x++) { dst[3 * x] = src[4 * x]; dst[3 * x + 1] = src[4 * x + 1]; dst[3 * x + 2] = src[4 * x + 2]; }
                return true;
            case DXGI_FORMAT_B8G8R8A8_UNORM: case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB: case DXGI_FORMAT_B8G8R8A8_TYPELESS:
            case DXGI_FORMAT_B8G8R8X8_UNORM: case DXGI_FORMAT_B8G8R8X8_UNORM_SRGB:
                for (int x = 0; x < w; x++) { dst[3 * x] = src[4 * x + 2]; dst[3 * x + 1] = src[4 * x + 1]; dst[3 * x + 2] = src[4 * x]; }
                return true;
            case DXGI_FORMAT_R10G10B10A2_UNORM: case DXGI_FORMAT_R10G10B10A2_TYPELESS:
                for (int x = 0; x < w; x++) {
                    uint32_t v;
                    memcpy(&v, src + 4 * x, 4);
                    for (int c = 0; c < 3; c++) dst[3 * x + c] = uint8_t(((v >> (10 * c)) & 0x3FF) >> 2);
                }
                return true;
            default: return false;
        }
    }

    // Render thread, inside Present after our overlay drew. Runs once per armed shot.
    void Tap(void* swapchain) {
        overlay::SetPresentTap(nullptr);
        Frame fr;
        auto* sc = static_cast<IDXGISwapChain*>(swapchain);
        ID3D11Texture2D* bb = nullptr;
        ID3D11Device* dev = nullptr;
        ID3D11DeviceContext* ctx = nullptr;
        ID3D11Texture2D* resolved = nullptr;
        ID3D11Texture2D* staging = nullptr;
        D3D11_TEXTURE2D_DESC d{};
        if (FAILED(sc->GetBuffer(0, __uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&bb))) || !bb) fr.err = "GetBuffer failed";
        else {
            bb->GetDesc(&d);
            bb->GetDevice(&dev);
            dev->GetImmediateContext(&ctx);
            ID3D11Texture2D* src = bb;
            if (d.SampleDesc.Count > 1) {  // MSAA back buffer: resolve first
                D3D11_TEXTURE2D_DESC r = d;
                r.SampleDesc = {1, 0};
                r.Usage = D3D11_USAGE_DEFAULT;
                r.BindFlags = 0;
                r.CPUAccessFlags = 0;
                r.MiscFlags = 0;
                if (SUCCEEDED(dev->CreateTexture2D(&r, nullptr, &resolved))) { ctx->ResolveSubresource(resolved, 0, bb, 0, d.Format); src = resolved; }
            }
            D3D11_TEXTURE2D_DESC s = d;
            s.MipLevels = s.ArraySize = 1;
            s.SampleDesc = {1, 0};
            s.Usage = D3D11_USAGE_STAGING;
            s.BindFlags = 0;
            s.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
            s.MiscFlags = 0;
            D3D11_MAPPED_SUBRESOURCE m{};
            if (FAILED(dev->CreateTexture2D(&s, nullptr, &staging))) fr.err = "staging texture failed";
            else {
                ctx->CopyResource(staging, src);
                if (FAILED(ctx->Map(staging, 0, D3D11_MAP_READ, 0, &m))) fr.err = "Map failed";
                else {
                    fr.w = int(d.Width);
                    fr.h = int(d.Height);
                    fr.rgb.resize(size_t(fr.w) * fr.h * 3);
                    for (int y = 0; y < fr.h && fr.err.empty(); y++)
                        if (!ToRgb(d.Format, static_cast<const uint8_t*>(m.pData) + size_t(y) * m.RowPitch, fr.w, fr.rgb.data() + size_t(y) * fr.w * 3))
                            fr.err = "back buffer format " + std::to_string(int(d.Format)) + " not supported";
                    ctx->Unmap(staging, 0);
                }
            }
        }
        for (IUnknown* u : std::initializer_list<IUnknown*>{staging, resolved, ctx, dev, bb}) if (u) u->Release();
        g_frame = std::move(fr);
        g_done = true;
    }

    std::string ModuleDir() {
        HMODULE self = nullptr;
        GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, reinterpret_cast<LPCSTR>(&Tap), &self);
        char buf[MAX_PATH] = {};
        GetModuleFileNameA(self, buf, MAX_PATH);
        const std::string p = buf;
        return p.substr(0, p.find_last_of("\\/") + 1);
    }
}

namespace live_bridge {
    std::string Shot(const std::vector<std::string>& w, const std::atomic<bool>& stop) {
        std::string path;
        size_t i = 1;
        if (w.size() == 2 || w.size() == 6) path = w[i++];
        int crop[4] = {0, 0, 0, 0};
        if (w.size() - i == 4) for (int k = 0; k < 4; k++) crop[k] = std::atoi(w[i + k].c_str());
        else if (w.size() != i) return Err("shot [path] [x y w h]");

        g_done = false;
        overlay::SetPresentTap(&Tap);
        for (int t = 0; t < 2000 && !g_done && !stop; t += 10) Sleep(10);
        if (!g_done) {
            overlay::SetPresentTap(nullptr);
            Sleep(100);  // a Present that already picked the tap up finishes it
            return Err("no frame presented within 2 s (minimised?)");
        }
        Frame fr = std::move(g_frame);
        if (!fr.err.empty()) return Err(fr.err);

        int x = crop[0], y = crop[1], cw = crop[2] ? crop[2] : fr.w, ch = crop[3] ? crop[3] : fr.h;
        if (x < 0 || y < 0 || cw <= 0 || ch <= 0 || x + cw > fr.w || y + ch > fr.h)
            return Err("crop outside the " + std::to_string(fr.w) + "x" + std::to_string(fr.h) + " frame");
        std::vector<uint8_t> rgb(size_t(cw) * ch * 3);
        for (int r = 0; r < ch; r++) memcpy(rgb.data() + size_t(r) * cw * 3, fr.rgb.data() + (size_t(y + r) * fr.w + x) * 3, size_t(cw) * 3);
        const std::vector<uint8_t> png = Png(cw, ch, rgb.data());

        if (path.empty()) {
            const std::string dir = ModuleDir() + "dos-tool-shots\\";
            CreateDirectoryA(dir.c_str(), nullptr);
            path = dir + std::to_string(GetTickCount64()) + ".png";
        }
        HANDLE h = CreateFileA(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h == INVALID_HANDLE_VALUE) return Err("cannot write " + path + " (error " + std::to_string(GetLastError()) + ")");
        DWORD wr = 0;
        const BOOL ok = WriteFile(h, png.data(), DWORD(png.size()), &wr, nullptr);
        CloseHandle(h);
        if (!ok || wr != png.size()) return Err("short write to " + path);
        return Ok("{\"path\":" + reflect::Esc(path) + ",\"size\":[" + std::to_string(cw) + "," + std::to_string(ch) + "],\"frame\":["
                  + std::to_string(fr.w) + "," + std::to_string(fr.h) + "],\"bytes\":" + std::to_string(png.size()) + "}");
    }
}

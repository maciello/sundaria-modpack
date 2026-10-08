#include <winsock2.h>  // before Windows.h (logger.hpp)
#include "live-bridge.hpp"
#include "feature.hpp"
#include "game.hpp"
#include "logger.hpp"
#include "overlay.hpp"
#include "umg.hpp"

#include <atomic>
#include <cstdio>

// Live bridge (#97, #98): agents query the running game over 127.0.0.1 TCP, one JSON line per command; host side
// `just game <cmd>` (scripts/game.py). Dev installs only (dos-tool.dev). No auth beyond loopback + the dev flag.
// Bridge thread: socket, log, shot, trace window. Game thread (our listener, world tick): get, find, call.
// Idle: the thread sleeps in select(); the ProcessEvent listener is registered only while a client is connected
// (and kLingerMs after its last command).
namespace {
    using namespace live_bridge;

    HANDLE g_thread = nullptr;
    std::atomic<bool> g_stop{false};              // set by Off(), read by the bridge thread
    std::atomic<bool> g_connected{false}, g_listening{false}, g_tracing{false};
    std::atomic<ULONGLONG> g_lastCmd{0};
    thread_local bool t_busy = false;             // our own UFunction calls re-enter ProcessEvent

    // One game-thread job at a time: 0 idle, 1 posted, 2 running, 3 done.
    std::atomic<int> g_job{0};
    std::string g_jobIn, g_jobOut;

    void OnEvent(void* obj, void* fn, void*) {
        if (t_busy || !game::OnGameThread()) return;
        if (g_tracing.load(std::memory_order_relaxed)) TraceEvent(obj, fn);
        if (g_job.load(std::memory_order_relaxed) != 1 || !umg::IsWorldTick(fn)) return;
        int posted = 1;
        if (!g_job.compare_exchange_strong(posted, 2)) return;
        t_busy = true;
        g_jobOut = RunOnGameThread(g_jobIn);
        t_busy = false;
        g_job = 3;
    }

    bool WaitListening() {  // the render thread registers the listener on its next frame
        for (int i = 0; i < 100 && !g_listening && !g_stop; i++) Sleep(10);
        return g_listening;
    }

    std::string Job(const std::string& line) {
        if (!WaitListening()) return Err("listener not registered: no frames presented (minimised?)");
        g_jobIn = line;
        g_job = 1;  // posted
        const ULONGLONG until = GetTickCount64() + kJobTimeoutMs;
        while (g_job != 3) {
            if (g_stop || GetTickCount64() > until) {
                int posted = 1;
                if (g_job.compare_exchange_strong(posted, 0))
                    return Err(g_stop ? "unloading" : "no world tick within " + std::to_string(kJobTimeoutMs) + " ms (main menu or loading?)");
            }
            Sleep(2);
        }
        std::string out = std::move(g_jobOut);
        g_job = 0;
        return out;
    }

    std::string Trace(const std::vector<std::string>& w) {
        if (w.size() < 2) return Err("trace <regex> [seconds]  (matched against Class::Function, case-insensitive)");
        double secs = w.size() > 2 ? std::atof(w[2].c_str()) : 5;
        secs = secs < 0.1 ? 0.1 : secs > kTraceMaxS ? kTraceMaxS : secs;
        std::string err;
        if (!TraceBegin(w[1], err)) return Err(err);
        if (!WaitListening()) return Err("listener not registered: no frames presented (minimised?)");
        g_tracing = true;
        const ULONGLONG until = GetTickCount64() + ULONGLONG(secs * 1000);
        while (GetTickCount64() < until && !g_stop) { Sleep(20); g_lastCmd = GetTickCount64(); }
        g_tracing = false;
        return TraceEnd(secs);
    }

    std::string LogPath() {
        HMODULE self = nullptr;
        GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, reinterpret_cast<LPCSTR>(&OnEvent), &self);
        char buf[MAX_PATH] = {};
        GetModuleFileNameA(self, buf, MAX_PATH);
        const std::string p = buf;
        return p.substr(0, p.find_last_of("\\/") + 1) + "dos-tool.log";
    }

    std::string LogTail(const std::vector<std::string>& w) {
        const int n = w.size() > 2 ? std::atoi(w[2].c_str()) : w.size() > 1 ? std::atoi(w[1].c_str()) : 40;  // "log tail 40" or "log 40"
        HANDLE h = CreateFileA(LogPath().c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
        if (h == INVALID_HANDLE_VALUE) return Err("no dos-tool.log");
        LARGE_INTEGER size{};
        GetFileSizeEx(h, &size);
        const LONGLONG from = size.QuadPart > (1 << 18) ? size.QuadPart - (1 << 18) : 0;  // last 256 KiB
        LARGE_INTEGER pos{};
        pos.QuadPart = from;
        SetFilePointerEx(h, pos, nullptr, FILE_BEGIN);
        std::string text(size_t(size.QuadPart - from), '\0');
        DWORD rd = 0;
        ReadFile(h, text.data(), DWORD(text.size()), &rd, nullptr);
        CloseHandle(h);
        text.resize(rd);
        std::vector<std::string> lines;
        for (size_t b = 0; b < text.size();) {
            size_t e = text.find('\n', b);
            if (e == std::string::npos) e = text.size();
            std::string l = text.substr(b, e - b);
            if (!l.empty() && l.back() == '\r') l.pop_back();
            lines.push_back(l);
            b = e + 1;
        }
        std::string s = "[";
        for (size_t i = lines.size() > size_t(n > 0 ? n : 40) ? lines.size() - (n > 0 ? n : 40) : 0; i < lines.size(); i++)
            s += (s.size() > 1 ? "," : "") + reflect::Esc(lines[i]);
        return Ok(s + "]");
    }

    std::string Handle(const std::string& line) {
        const std::vector<std::string> w = Words(line);
        if (w.empty()) return Err("empty command");
        const std::string& c = w[0];
        if (c == "get" || c == "find" || c == "call") return Job(line);
        if (c == "trace") return Trace(w);
        if (c == "shot") return Shot(w, g_stop);
        if (c == "log") return LogTail(w);
        if (c == "ping") return Ok("{\"listening\":" + std::string(g_listening ? "true" : "false") + ",\"port\":" + std::to_string(kPort) + "}");
        return Err("commands: get <path> [depth] | find <class> [near <m>] | call <path> <Function> [json args] | "
                   "trace <regex> [s] | shot [path] [x y w h] | log [n] | ping");
    }

    bool Readable(SOCKET s, int ms) {
        fd_set r;
        FD_ZERO(&r);
        FD_SET(s, &r);
        timeval tv{0, ms * 1000};
        return select(0, &r, nullptr, nullptr, &tv) > 0;
    }

    void Session(SOCKET c) {
        std::string buf;
        char chunk[4096];
        while (!g_stop) {
            if (!Readable(c, 250)) continue;
            const int n = recv(c, chunk, sizeof chunk, 0);
            if (n <= 0) return;
            buf.append(chunk, n);
            for (size_t nl; (nl = buf.find('\n')) != std::string::npos;) {
                const std::string line = buf.substr(0, nl);
                buf.erase(0, nl + 1);
                g_lastCmd = GetTickCount64();
                const std::string out = Handle(line) + "\n";
                g_lastCmd = GetTickCount64();
                for (size_t sent = 0; sent < out.size();) {
                    const int k = send(c, out.data() + sent, int(out.size() - sent), 0);
                    if (k <= 0) return;
                    sent += size_t(k);
                }
            }
            if (buf.size() > (1 << 16)) return;  // no newline in 64 KiB: not our client
        }
    }

    SOCKET Listen() {
        SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (s == INVALID_SOCKET) return s;
        BOOL excl = TRUE;
        setsockopt(s, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, reinterpret_cast<const char*>(&excl), sizeof excl);
        sockaddr_in a{};
        a.sin_family = AF_INET;
        a.sin_port = htons(kPort);
        a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);  // never outside loopback
        if (bind(s, reinterpret_cast<sockaddr*>(&a), sizeof a) != 0 || listen(s, 2) != 0) { closesocket(s); return INVALID_SOCKET; }
        return s;
    }

    DWORD WINAPI Serve(LPVOID) {
        WSADATA wsa;
        WSAStartup(MAKEWORD(2, 2), &wsa);
        SOCKET ls = INVALID_SOCKET;
        bool told = false;
        while (!g_stop) {
            if (ls == INVALID_SOCKET) {
                ls = Listen();
                if (ls == INVALID_SOCKET) {
                    if (!told) logger::log("[live-bridge] 127.0.0.1:" + std::to_string(kPort) + " busy, retrying every second");
                    told = true;
                    for (int i = 0; i < 4 && !g_stop; i++) Sleep(250);
                    continue;
                }
                logger::log("[live-bridge] listening on 127.0.0.1:" + std::to_string(kPort));
            }
            if (!Readable(ls, 250)) continue;
            SOCKET c = accept(ls, nullptr, nullptr);
            if (c == INVALID_SOCKET) continue;
            g_connected = true;
            Session(c);
            closesocket(c);
            g_connected = false;
        }
        if (ls != INVALID_SOCKET) closesocket(ls);
        WSACleanup();
        return 0;
    }

    struct LiveBridge : feature::Feature {
        int dev = -1;  // dos-tool.dev next to the exe: checked once
        LiveBridge() : Feature("Live bridge", feature::Stage::Alpha) {}

        // Render thread: starts the bridge thread once; (un)registers the listener as clients come and go.
        void OnFrame(const feature::Frame&) override {
            if (dev < 0) {
                char p[MAX_PATH] = {};
                GetModuleFileNameA(nullptr, p, MAX_PATH);
                std::string s = p;
                dev = GetFileAttributesA((s.substr(0, s.find_last_of("\\/") + 1) + "dos-tool.dev").c_str()) != INVALID_FILE_ATTRIBUTES;
            }
            if (!dev) return;
            if (!g_thread) {
                g_stop = false;
                g_thread = CreateThread(nullptr, 0, Serve, nullptr, 0, nullptr);
            }
            const bool want = g_connected || g_tracing || GetTickCount64() - g_lastCmd.load() < kLingerMs;
            if (want != g_listening) {
                game::SetEventListener(&OnEvent, want);
                g_listening = want;
            }
        }

        void Off() override {
            if (!g_thread) return;
            g_stop = true;
            if (WaitForSingleObject(g_thread, 5000) != WAIT_OBJECT_0) logger::log("[live-bridge] thread did not exit in 5 s");
            CloseHandle(g_thread);
            g_thread = nullptr;
            overlay::SetPresentTap(nullptr);
            if (g_listening) game::SetEventListener(&OnEvent, false);
            g_listening = g_connected = g_tracing = false;
            g_job = 0;
            logger::log("[live-bridge] closed");
        }
    } g_live_bridge;
}

#pragma once
// Live bridge, SDK-free parts: command words, property paths, JSON call args, the PNG writer. test/live-bridge_test.cpp.
#include <atomic>
#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <string>
#include <vector>
#include "reflect.hpp"

namespace live_bridge {
    constexpr int kPort = 47811;            // 127.0.0.1 only
    constexpr int kJobTimeoutMs = 3000;     // game-thread commands: no world tick in time = error
    constexpr int kTraceMaxS = 60;
    constexpr unsigned kLingerMs = 10000;   // listener stays registered this long after the last command

    // One response line each: {"ok":true,"result":<json>} or {"ok":false,"error":"..."}.
    inline std::string Ok(const std::string& json) { return "{\"ok\":true,\"result\":" + json + "}"; }
    inline std::string Err(const std::string& msg) { return "{\"ok\":false,\"error\":" + reflect::Esc(msg) + "}"; }

    // world.cpp, game thread (bridge listener): get / find / call -> response line; the trace.
    std::string RunOnGameThread(const std::string& line);
    bool TraceBegin(const std::string& pattern, std::string& err);  // bridge thread, before tracing goes on
    void TraceEvent(void* obj, void* fn);                            // game thread, every ProcessEvent while on
    std::string TraceEnd(double seconds);                            // bridge thread, after tracing went off
    // shot.cpp, bridge thread: shot [path] [x y w h] -> response line. stop: give up waiting for a frame.
    std::string Shot(const std::vector<std::string>& words, const std::atomic<bool>& stop);

    // Whitespace-separated words; the n-th word's start offset in `rest` (for "rest of line" arguments).
    inline std::vector<std::string> Words(const std::string& s, std::vector<size_t>* at = nullptr) {
        std::vector<std::string> w;
        for (size_t i = 0; i < s.size();) {
            while (i < s.size() && (s[i] == ' ' || s[i] == '\t' || s[i] == '\r' || s[i] == '\n')) i++;
            if (i >= s.size()) break;
            const size_t b = i;
            while (i < s.size() && !(s[i] == ' ' || s[i] == '\t' || s[i] == '\r' || s[i] == '\n')) i++;
            w.push_back(s.substr(b, i - b));
            if (at) at->push_back(b);
        }
        return w;
    }

    // "feature" (list) | "feature <name> on|off"; the name may hold spaces. on: -1 = list.
    struct FeatureCmd { std::string name; int on = -1; std::string err; };
    inline FeatureCmd ParseFeature(const std::string& line) {
        std::vector<size_t> at;
        const std::vector<std::string> w = Words(line, &at);
        FeatureCmd c;
        if (w.size() < 2) return c;
        if (w.size() < 3 || (w.back() != "on" && w.back() != "off")) { c.err = "feature [<name> on|off]"; return c; }
        c.on = w.back() == "on";
        c.name = line.substr(at[1], at.back() - at[1]);
        while (!c.name.empty() && (c.name.back() == ' ' || c.name.back() == '\t')) c.name.pop_back();
        return c;
    }
    // Case-insensitive exact name; -1 = none.
    inline int FindName(const std::vector<std::string>& names, const std::string& q) {
        for (size_t i = 0; i < names.size(); i++) {
            if (names[i].size() != q.size()) continue;
            size_t k = 0;
            while (k < q.size() && std::tolower(static_cast<unsigned char>(names[i][k])) == std::tolower(static_cast<unsigned char>(q[k]))) k++;
            if (k == q.size()) return int(i);
        }
        return -1;
    }

    // "root.Name.Arr[2].Field": root = first segment ("pawn", "@3", "obj:Name" …⊇), then reflect steps.
    struct Path { std::string root; std::vector<reflect::Step> steps; std::string err; };
    inline Path ParsePath(const std::string& s) {
        Path p;
        size_t i = 0;
        const auto seg = [&](std::string& name, int& index) -> bool {
            const size_t b = i;
            while (i < s.size() && s[i] != '.' && s[i] != '[') i++;
            name = s.substr(b, i - b);
            if (name.empty()) { p.err = "empty name at " + std::to_string(b); return false; }
            index = -1;
            if (i < s.size() && s[i] == '[') {
                const size_t close = s.find(']', i);
                if (close == std::string::npos || close == i + 1) { p.err = "bad index at " + std::to_string(i); return false; }
                char* end = nullptr;
                const long v = std::strtol(s.c_str() + i + 1, &end, 10);
                if (end != s.c_str() + close || v < 0) { p.err = "bad index at " + std::to_string(i); return false; }
                index = int(v);
                i = close + 1;
            }
            if (i < s.size() && s[i] != '.') { p.err = "expected . at " + std::to_string(i); return false; }
            return true;
        };
        int rootIndex;
        if (!seg(p.root, rootIndex)) return p;
        if (rootIndex >= 0) { p.err = "index on the root"; return p; }
        while (i < s.size()) {
            i++;  // '.'
            reflect::Step st;
            if (!seg(st.name, st.index)) return p;
            p.steps.push_back(st);
        }
        return p;
    }

    // Trace pattern: a regex subset, case-insensitive, unanchored: a|b alternatives, ^ $ . and x* (Pike's matcher).
    // std::regex is avoided: it imports msvcp140 locale facets, and Proton's older msvcp140 has crashed us before.
    namespace detail {
        inline bool Eq(char p, char c) { return p == '.' || std::tolower(static_cast<unsigned char>(p)) == std::tolower(static_cast<unsigned char>(c)); }
        inline bool Here(const char* p, const char* pe, const char* t);
        inline bool Star(char c, const char* p, const char* pe, const char* t) {
            do { if (Here(p, pe, t)) return true; } while (*t && Eq(c, *t++));
            return false;
        }
        inline bool Here(const char* p, const char* pe, const char* t) {
            if (p == pe) return true;
            if (p + 1 < pe && p[1] == '*') return Star(p[0], p + 2, pe, t);
            if (p[0] == '$' && p + 1 == pe) return !*t;
            return *t && Eq(p[0], *t) && Here(p + 1, pe, t + 1);
        }
    }
    inline bool Match(const std::string& pattern, const std::string& text) {
        for (size_t b = 0;;) {
            size_t e = pattern.find('|', b);
            if (e == std::string::npos) e = pattern.size();
            const char* p = pattern.c_str() + b;
            const char* pe = pattern.c_str() + e;
            if (p < pe && *p == '^') { if (detail::Here(p + 1, pe, text.c_str())) return true; }
            else for (const char* t = text.c_str();; t++) { if (detail::Here(p, pe, t)) return true; if (!*t) break; }
            if (e == pattern.size()) return false;
            b = e + 1;
        }
    }

    // Root aliases: pawn ps cam hud = the local controller's, gi gs = the world's.
    inline void ExpandAlias(Path& p) {
        static const struct { const char* alias; const char* root; const char* step; } kAlias[] = {
            {"pawn", "pc", "Pawn"}, {"ps", "pc", "PlayerState"}, {"cam", "pc", "PlayerCameraManager"}, {"hud", "pc", "MyHUD"},
            {"gi", "world", "OwningGameInstance"}, {"gs", "world", "GameState"}};
        for (const auto& a : kAlias)
            if (p.root == a.alias) {
                p.root = a.root;
                p.steps.insert(p.steps.begin(), reflect::Step{a.step, -1});
                return;
            }
    }

    // Call args: a JSON array of scalars ([1.5, "Fire", true]), or one scalar, or nothing. false: err.
    inline bool ParseArgs(const std::string& s, std::vector<reflect::Scalar>& out, std::string& err) {
        size_t i = 0;
        const auto ws = [&] { while (i < s.size() && (s[i] == ' ' || s[i] == '\t' || s[i] == '\r' || s[i] == '\n')) i++; };
        const auto scalar = [&]() -> bool {
            ws();
            reflect::Scalar v;
            if (i < s.size() && s[i] == '"') {
                v.isStr = true;
                for (i++; i < s.size() && s[i] != '"'; i++) {
                    if (s[i] == '\\' && i + 1 < s.size()) i++;
                    v.str += s[i];
                }
                if (i >= s.size()) { err = "unterminated string"; return false; }
                i++;
            } else if (!s.compare(i, 4, "true")) { v.num = 1; i += 4; }
            else if (!s.compare(i, 5, "false")) { v.num = 0; i += 5; }
            else {
                char* end = nullptr;
                v.num = std::strtod(s.c_str() + i, &end);
                if (end == s.c_str() + i) { err = "expected a number, string or bool at " + std::to_string(i); return false; }
                i = end - s.c_str();
            }
            out.push_back(v);
            return true;
        };
        ws();
        if (i >= s.size()) return true;
        if (s[i] != '[') { if (!scalar()) return false; ws(); if (i < s.size()) { err = "trailing text at " + std::to_string(i); return false; } return true; }
        i++;
        ws();
        if (i < s.size() && s[i] == ']') i++;
        else
            for (;;) {
                if (!scalar()) return false;
                ws();
                if (i < s.size() && s[i] == ',') { i++; continue; }
                if (i < s.size() && s[i] == ']') { i++; break; }
                err = "expected , or ] at " + std::to_string(i);
                return false;
            }
        ws();
        if (i < s.size()) { err = "trailing text at " + std::to_string(i); return false; }
        return true;
    }

    // PNG, 8-bit RGB, deflate "stored" blocks (no compression): no library needed, ~3 bytes per pixel.
    inline uint32_t Crc(const uint8_t* p, size_t n, uint32_t c = 0) {
        static uint32_t t[256] = {};
        if (!t[1])
            for (uint32_t i = 0; i < 256; i++) {
                uint32_t v = i;
                for (int k = 0; k < 8; k++) v = v & 1 ? 0xEDB88320u ^ (v >> 1) : v >> 1;
                t[i] = v;
            }
        c = ~c;
        for (size_t i = 0; i < n; i++) c = t[(c ^ p[i]) & 0xFF] ^ (c >> 8);
        return ~c;
    }
    inline uint32_t Adler(const uint8_t* p, size_t n) {
        uint32_t a = 1, b = 0;
        for (size_t i = 0; i < n; i++) { a = (a + p[i]) % 65521; b = (b + a) % 65521; }
        return b << 16 | a;
    }
    inline std::vector<uint8_t> Png(int w, int h, const uint8_t* rgb) {
        std::vector<uint8_t> raw;  // each row: filter 0 + pixels
        raw.reserve(size_t(h) * (size_t(w) * 3 + 1));
        for (int y = 0; y < h; y++) {
            raw.push_back(0);
            raw.insert(raw.end(), rgb + size_t(y) * w * 3, rgb + size_t(y + 1) * w * 3);
        }
        std::vector<uint8_t> z = {0x78, 0x01};
        for (size_t off = 0; off < raw.size() || off == 0;) {
            const size_t n = raw.size() - off < 65535 ? raw.size() - off : 65535;
            z.push_back(off + n == raw.size() ? 1 : 0);
            z.push_back(uint8_t(n)); z.push_back(uint8_t(n >> 8));
            z.push_back(uint8_t(~n)); z.push_back(uint8_t(~n >> 8));
            z.insert(z.end(), raw.begin() + off, raw.begin() + off + n);
            off += n;
            if (!n) break;
        }
        const uint32_t ad = Adler(raw.data(), raw.size());
        for (int k = 3; k >= 0; k--) z.push_back(uint8_t(ad >> (8 * k)));

        std::vector<uint8_t> out = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n'};
        const auto chunk = [&](const char* type, const std::vector<uint8_t>& d) {
            for (int k = 3; k >= 0; k--) out.push_back(uint8_t(d.size() >> (8 * k)));
            const size_t at = out.size();
            out.insert(out.end(), type, type + 4);
            out.insert(out.end(), d.begin(), d.end());
            const uint32_t c = Crc(out.data() + at, d.size() + 4);
            for (int k = 3; k >= 0; k--) out.push_back(uint8_t(c >> (8 * k)));
        };
        std::vector<uint8_t> ihdr;
        for (int v : {w, h}) for (int k = 3; k >= 0; k--) ihdr.push_back(uint8_t(uint32_t(v) >> (8 * k)));
        ihdr.insert(ihdr.end(), {8, 2, 0, 0, 0});  // 8 bit, RGB, deflate, filter 0, no interlace
        chunk("IHDR", ihdr);
        chunk("IDAT", z);
        chunk("IEND", {});
        return out;
    }
}

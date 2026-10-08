#include "layout.hpp"

#include <Windows.h>

namespace hub_layout {
    std::string GamePath(const char* file) {
        char buf[MAX_PATH] = {};
        GetModuleFileNameA(nullptr, buf, MAX_PATH);
        std::string p = buf;
        return p.substr(0, p.find_last_of("\\/") + 1) + file;
    }

    // Win32 file I/O like logger.hpp: no <fstream> (keeps the DLL off msvcp140's stream code under Proton).
    std::string ReadText(const std::string& path) {
        std::string out;
        HANDLE h = CreateFileA(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
        if (h == INVALID_HANDLE_VALUE) return out;
        char buf[4096];
        DWORD n = 0;
        while (ReadFile(h, buf, sizeof(buf), &n, nullptr) && n > 0) out.append(buf, n);
        CloseHandle(h);
        return out;
    }

    void WriteText(const std::string& path, const std::string& text) {
        HANDLE h = CreateFileA(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h == INVALID_HANDLE_VALUE) return;
        DWORD n = 0;
        WriteFile(h, text.data(), DWORD(text.size()), &n, nullptr);
        CloseHandle(h);
    }

    std::vector<Spot>& Spots() {
        static std::vector<Spot> spots = ParseLayout(ReadText(GamePath("dos-tool-tavern.ini")));
        return spots;
    }

    void Save() { WriteText(GamePath("dos-tool-tavern.ini"), WriteLayout(Spots())); }
}

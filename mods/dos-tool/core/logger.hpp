#pragma once
#include <string>
#include <Windows.h>

// Minimal logger via the Win32 API (no <fstream>, so the static CRT link has no
// dllimport-stdio dependency). Writes dos-tool.log next to DoS-Tool.asi.
namespace logger {
    inline void log(const std::string& msg) {
        static bool first = true;
        static std::string path = [] {
            HMODULE self = nullptr;
            GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                               reinterpret_cast<LPCSTR>(&log), &self);
            char buf[MAX_PATH] = {};
            GetModuleFileNameA(self, buf, MAX_PATH);
            std::string p = buf;
            return p.substr(0, p.find_last_of("\\/") + 1) + "dos-tool.log";
        }();
        HANDLE h = CreateFileA(path.c_str(),
            FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
            first ? CREATE_ALWAYS : OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        first = false;
        if (h == INVALID_HANDLE_VALUE) return;
        SetFilePointer(h, 0, nullptr, FILE_END);
        std::string line = msg;
        line += "\r\n";
        DWORD written = 0;
        WriteFile(h, line.data(), (DWORD)line.size(), &written, nullptr);
        CloseHandle(h);
    }
}

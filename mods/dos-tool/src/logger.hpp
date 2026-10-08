#pragma once
#include <string>
#include <Windows.h>

// Minimal logger via the Win32 API (no <fstream>, so the static CRT link has no
// dllimport-stdio dependency). Writes to a Linux-visible path via Wine's Z: drive
// so we can read proof-of-life output from the Linux side.
namespace logger {
    inline void log(const std::string& msg) {
        static bool first = true;
        HANDLE h = CreateFileA("Z:\\home\\rob\\Code\\dos-tool\\dos-tool.log",
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

// DoS-Tool.asi: loads DoS-Tool.dll. With dos-tool.dev next to it, reloads the dll whenever it changes.
#include <Windows.h>
#include <string>

static std::string Dir(HMODULE m) {
    char b[MAX_PATH] = {};
    GetModuleFileNameA(m, b, MAX_PATH);
    std::string p = b;
    return p.substr(0, p.find_last_of("\\/") + 1);
}

static DWORD WINAPI Watch(LPVOID self) {
    const std::string dir = Dir(static_cast<HMODULE>(self)), src = dir + "DoS-Tool.dll";
    const bool dev = GetFileAttributesA((dir + "dos-tool.dev").c_str()) != INVALID_FILE_ATTRIBUTES;
    HMODULE mod = nullptr;
    FILETIME last{};
    int gen = 0;
    for (;;) {
        WIN32_FILE_ATTRIBUTE_DATA a;
        if (GetFileAttributesExA(src.c_str(), GetFileExInfoStandard, &a) && CompareFileTime(&a.ftLastWriteTime, &last) != 0) {
            last = a.ftLastWriteTime;
            if (mod) {
                if (auto stop = (void (*)())GetProcAddress(mod, "ModStop")) stop();
                FreeLibrary(mod);
                mod = nullptr;
            }
            std::string path = src;
            if (dev) {  // load a copy so the build can overwrite DoS-Tool.dll
                Sleep(300);
                path = dir + "DoS-Tool.loaded" + std::to_string(gen++ % 2) + ".dll";
                if (!CopyFileA(src.c_str(), path.c_str(), FALSE)) continue;
            }
            if ((mod = LoadLibraryA(path.c_str())))
                if (auto start = (void (*)())GetProcAddress(mod, "ModStart")) start();
        }
        if (!dev) return 0;
        Sleep(500);
    }
}

BOOL APIENTRY DllMain(HMODULE self, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(self);
        CreateThread(nullptr, 0, Watch, self, 0, nullptr);
    }
    return TRUE;
}

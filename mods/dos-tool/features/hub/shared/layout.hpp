#pragma once
#include <string>
#include <vector>
#include "layout_text.hpp"

// The hub layout, loaded once from dos-tool-tavern.ini next to the game exe and shared by the hub features
// (render thread only). Plus the Win32 file helpers they use (no <fstream>: keeps the DLL off msvcp140's stream
// code under Proton, like logger.hpp).
namespace hub_layout {
    std::vector<Spot>& Spots();  // loaded on first use
    void Save();
    std::string GamePath(const char* file);  // next to the game exe
    std::string ReadText(const std::string& path);
    void WriteText(const std::string& path, const std::string& text);
}

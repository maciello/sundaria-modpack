#pragma once
#include <cstdio>
#include "imgui.h"

// Drawing helpers shared by features.
namespace draw {
    inline void FormatAmount(char* buf, size_t n, double v) {
        if (v >= 1e6)      std::snprintf(buf, n, "%.1fM", v / 1e6);
        else if (v >= 1e4) std::snprintf(buf, n, "%.1fk", v / 1e3);
        else               std::snprintf(buf, n, "%.0f", v);
    }

    inline void OutlinedText(ImDrawList* dl, ImFont* f, float size, ImVec2 p, ImU32 col, ImU32 outline, float w, const char* s) {
        for (int dx = -1; dx <= 1; dx++)
            for (int dy = -1; dy <= 1; dy++)
                if (dx || dy) dl->AddText(f, size, ImVec2(p.x + dx * w, p.y + dy * w), outline, s);
        dl->AddText(f, size, p, col, s);
    }
}

#include "hud.hpp"
#include "draw.hpp"
#include "style.hpp"
#include "imgui.h"

#include <algorithm>
#include <cstdio>
#include <vector>

namespace {
    struct Line { std::string text; ImFont* font; float size; std::uint32_t color; };
}

void build_hud::Draw(const feature::Frame& f, const State& s) {
    using namespace style;
    const float ui = type::Ui(f.h), pad = space::k5 * ui, gap = space::k2 * ui;
    ImFont* title = f.font ? f.font : ImGui::GetFont();
    ImFont* body = ImGui::GetFont();
    const std::uint32_t soft = Pack(color::kTextSoft), muted = Pack(color::kTextMuted);
    const char* pick = s.flying ? "Left click" : "E / left click";

    std::vector<Line> lines;
    lines.push_back({"Build mode", title, type::kMd * ui, Pack(color::kAccent)});
    char buf[256];
    if (!s.carried.empty()) std::snprintf(buf, sizeof(buf), "Carrying: %s", s.carried.c_str());
    else std::snprintf(buf, sizeof(buf), "Aiming at: %s", s.target.empty() ? "-" : s.target.c_str());
    lines.push_back({buf, body, type::kSm * ui, soft});
    std::snprintf(buf, sizeof(buf), "New (N): %s  [%d/%d]", s.catalogCount ? s.catalog.c_str() : "-", s.catalogCount ? s.catalogIndex + 1 : 0,
                  s.catalogCount);
    lines.push_back({buf, body, type::kSm * ui, soft});
    if (!s.carried.empty()) {
        std::snprintf(buf, sizeof(buf), "%s  put down    R  turn    Wheel / PgUp PgDn  height", pick);
        lines.push_back({buf, body, type::kXs * ui, muted});
        lines.push_back({s.flying ? "Backspace  cancel" : "Right click / Backspace  cancel", body, type::kXs * ui, muted});
    } else {
        std::snprintf(buf, sizeof(buf), "%s  pick up    T  fetch here    C  copy", pick);
        lines.push_back({buf, body, type::kXs * ui, muted});
        lines.push_back({"[ ]  catalog    N  place new    Del  reset    B  leave", body, type::kXs * ui, muted});
    }

    float w = 0, h = 0;
    for (const Line& l : lines) {
        const ImVec2 sz = l.font->CalcTextSizeA(l.size, 1e6f, 0.0f, l.text.c_str());
        w = std::max(w, sz.x);
        h += sz.y + gap;
    }
    h -= gap;
    const float margin = space::k6 * ui;
    const ImVec2 p0{margin, f.h - margin - h - 2 * pad}, p1{margin + w + 2 * pad, f.h - margin};
    ImDrawList* dl = ImGui::GetBackgroundDrawList();
    dl->AddRectFilled(p0, p1, Pack(color::kPanel, 0.45f / (color::kPanel.a / 255.0f)), radius::kMd * ui);
    float y = p0.y + pad;
    for (const Line& l : lines) {
        draw::OutlinedText(dl, l.font, l.size, ImVec2(p0.x + pad, y), l.color, Pack(color::kInk, stroke::kOutlineAlpha),
                           stroke::Outline(l.size) * 0.6f, l.text.c_str());
        y += l.font->CalcTextSizeA(l.size, 1e6f, 0.0f, l.text.c_str()).y + gap;
    }
}

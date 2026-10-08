#pragma once
#include <string>
#include "feature.hpp"

// Build mode's HUD panel: what is targeted / carried, the catalog pick, and the keys. Spec: design-system.md
// "Menu (Insert window) and HUD panels" (DPS meter pattern) - panel kPanel @ .45, radius kMd, margin k6·Ui; title in
// the display font at kMd in kAccent; lines kSm kTextSoft, key hints kTextMuted; every glyph ink-outlined.
// Unverified visually.
namespace build_hud {
    struct State {
        std::string target;   // what the crosshair is on ("" = nothing)
        std::string carried;  // what you carry ("" = nothing)
        std::string catalog;  // the catalog item N would spawn
        int catalogIndex = 0, catalogCount = 0;
        bool flying = false;  // free camera: left click instead of E
    };
    void Draw(const feature::Frame& f, const State& s);
}

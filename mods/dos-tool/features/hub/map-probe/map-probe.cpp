#include "feature.hpp"
#include "../shared/map_probe.hpp"
#include "imgui.h"

// Map probe (dev): idle until dos-tool-mapsurvey.txt appears next to the game exe; see hub/shared/map_probe.hpp.
namespace {
    struct MapProbe : feature::Feature {
        MapProbe() : Feature("Map probe", feature::Stage::Alpha) {}
        void OnFrame(const feature::Frame&) override { map_probe::Tick(); }
        void Menu() override { ImGui::TextDisabled("create dos-tool-mapsurvey.txt next to the game exe (in the hub)"); }
    } g_mapProbe;
}

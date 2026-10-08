#include "feature.hpp"
#include "../shared/mesh_probe.hpp"
#include "imgui.h"

// Mesh probe (dev): idle until dos-tool-meshes.txt appears next to the game exe; see hub/shared/mesh_probe.hpp.
namespace {
    struct MeshProbe : feature::Feature {
        MeshProbe() : Feature("Mesh probe", feature::Stage::Alpha) {}
        void OnFrame(const feature::Frame&) override { mesh_probe::Tick(); }
        void Menu() override { ImGui::TextDisabled("search terms in dos-tool-meshes.txt next to the game exe"); }
    } g_meshProbe;
}

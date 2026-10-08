#include "feature.hpp"
#include "imgui.h"

namespace {
    struct Camera : feature::Feature {
        bool seeded = false;
        bool ovrFov = false;  float fov = 90.0f;
        bool ovrDist = false; float dist = 650.0f;
        bool noCollision = false;

        Camera() : Feature("Camera", true) {}

        void OnFrame(const feature::Frame& f) override {
            if (!seeded && f.snap.haveCamera) {
                fov = game::OriginalFOV();
                dist = game::OriginalDistance();
                seeded = true;
            }
            if (ovrFov)  game::SetFOV(fov);
            if (ovrDist) game::SetCameraDistance(dist);
            game::SetCameraCollision(!noCollision);
        }

        void Off() override {
            if (!seeded) return;
            game::SetFOV(game::OriginalFOV());
            game::SetCameraDistance(game::OriginalDistance());
            game::SetCameraCollision(true);
        }

        void Menu() override {
            ImGui::Checkbox("Override FOV", &ovrFov);
            ImGui::SameLine(); ImGui::SetNextItemWidth(150);
            ImGui::SliderFloat("##fov", &fov, 40.0f, 130.0f, "%.0f");
            ImGui::Checkbox("Override distance", &ovrDist);
            ImGui::SameLine(); ImGui::SetNextItemWidth(150);
            ImGui::SliderFloat("##dist", &dist, 150.0f, 2200.0f, "%.0f");
            ImGui::Checkbox("Disable camera collision (no zoom-in at walls)", &noCollision);
        }
    } g_camera;
}

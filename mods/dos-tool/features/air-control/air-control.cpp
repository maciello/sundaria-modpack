#include "feature.hpp"
#include "imgui.h"

// Overwatch-style air strafing: UE's AirControl is the fraction of ground acceleration
// available while falling (engine default 0.05). Movement is server-simulated, so in co-op
// the host's values apply; clients without the pack get corrected back.
namespace {
    struct AirControl : feature::Feature {
        game::Movement m{0.8f, 2.0f, 25.0f, 0.0f, 0.0f};
        bool seeded = false;

        AirControl() : Feature("Air control", feature::Stage::Beta) {}

        void OnFrame(const feature::Frame&) override {
            game::ApplyMovement(&m);
            game::Movement o;
            if (!seeded && game::OriginalMovement(o)) {
                m.boostMultiplier = o.boostMultiplier;
                m.boostThreshold = o.boostThreshold;
                m.lateralFriction = o.lateralFriction;
                m.brakingFalling = o.brakingFalling;
                seeded = true;
            }
        }

        void Off() override { game::ApplyMovement(nullptr); }

        void Menu() override {
            ImGui::SliderFloat("Air control", &m.airControl, 0.0f, 1.5f, "%.2f");
            ImGui::SliderFloat("Air brake", &m.brakingFalling, 0.0f, 2000.0f, "%.0f");
            ImGui::SliderFloat("Air friction", &m.lateralFriction, 0.0f, 4.0f, "%.2f");
            game::Movement o;
            if (game::OriginalMovement(o))
                ImGui::TextDisabled("vanilla: control %.2f  brake %.0f  friction %.2f", o.airControl, o.brakingFalling, o.lateralFriction);
        }
    } g_air;
}

#include "feature.hpp"
#include "ow-movement.hpp"
#include "imgui.h"
#include <algorithm>

// Overwatch movement: near-instant ground acceleration and stopping, optional higher jump.
// Movement is server-simulated: applied to every player character (each blended from its own
// vanilla values), the host's values win in co-op. Air movement stays with Air control.
namespace {
    struct Settings { float snap = 1.0f, jump = 1.0f; };

    game::Ground Apply(const game::Ground& v, const void* ctx) {
        const auto* s = static_cast<const Settings*>(ctx);
        const auto g = ow_movement::Blend({v.maxAccel, v.brakingWalking, v.groundFriction, v.brakingFrictionFactor, v.jumpZ},
                                          s->snap, s->jump);
        return {g.maxAccel, g.brakingWalking, g.groundFriction, g.brakingFrictionFactor, g.jumpZ};
    }

    struct OwMovement : feature::Feature {
        Settings set;
        float topSpeed = 0.0f;

        OwMovement() : Feature("Overwatch movement", feature::Stage::Alpha) {}

        void OnFrame(const feature::Frame&) override {
            game::ApplyGround(&Apply, &set);
            topSpeed = std::max(topSpeed, game::LocalSpeed());
        }

        void Off() override { game::ApplyGround(nullptr, nullptr); }

        void Menu() override {
            ImGui::SliderFloat("Snappiness", &set.snap, 0.0f, 1.0f, "%.2f");
            ImGui::SliderFloat("Jump height", &set.jump, 0.5f, 2.0f, "x%.2f");
            game::Ground o;
            if (!game::OriginalGround(o)) { ImGui::TextDisabled("not in gameplay"); return; }
            const game::Ground g = Apply(o, &set);
            ImGui::TextDisabled("speed %.0f  full speed in %.2f s (vanilla %.2f s)", game::LocalSpeed(),
                                ow_movement::TimeTo(topSpeed, g.maxAccel), ow_movement::TimeTo(topSpeed, o.maxAccel));
            ImGui::TextDisabled("vanilla: accel %.0f  braking %.0f  friction %.1f  jump %.0f",
                                o.maxAccel, o.brakingWalking, o.groundFriction, o.jumpZ);
        }
    } g_ow_movement;
}

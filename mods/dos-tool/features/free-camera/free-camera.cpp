#include "feature.hpp"
#include "free-camera.hpp"
#include "logger.hpp"
#include "imgui.h"

#include <Windows.h>
#include <cstdio>
#include <string>

// Free camera: a hotkey (default F6; F8 is the game's HUD toggle) detaches the view and flies it (WASD, Space/E up, Ctrl/Q down, Shift fast,
// hold right mouse or arrow keys to look). First test for a walkable hub: how much of the village exists.
namespace {
    bool Down(int vk) { return (GetAsyncKeyState(vk) & 0x8000) != 0; }

    struct Key { int vk; const char* name; };
    constexpr Key kKeys[] = {{VK_F6, "F6"}, {VK_F7, "F7"}, {VK_F9, "F9"}, {VK_F10, "F10"}, {VK_F11, "F11"},
                             {VK_NUMPAD0, "Numpad 0"}, {VK_HOME, "Home"}, {VK_END, "End"}};

    bool GameFocused() {
        DWORD pid = 0;
        GetWindowThreadProcessId(GetForegroundWindow(), &pid);
        return pid == GetCurrentProcessId();
    }

    struct FreeCamera : feature::Feature {
        bool flying = false, keyWas = false, looking = false;
        int key = 0;  // index into kKeys
        POINT anchor{};
        float speed = 800.0f, sens = 0.15f;
        free_camera::Pose pose{};
        double last = 0;
        std::string manager = "-", target = "-";

        FreeCamera() : Feature("Free camera", feature::Stage::Alpha) {}

        void Start() {
            combat::View v{};
            if (!game::GetView(v)) { logger::log("[freecam] no camera view yet"); return; }
            pose = {v.x, v.y, v.z, v.pitch, v.yaw};
            flying = true;
            game::CameraClasses(manager, target);
            char buf[256];
            std::snprintf(buf, sizeof(buf), "[freecam] on at %.0f %.0f %.0f, camera %s, view target %s",
                          v.x, v.y, v.z, manager.c_str(), target.c_str());
            logger::log(buf);
        }

        void Stop() {
            if (flying) {
                char buf[96];
                std::snprintf(buf, sizeof(buf), "[freecam] off, overrides so far %d", game::FreeCamOverrides());
                logger::log(buf);
            }
            flying = false;
            looking = false;
            game::SetFreeCam(nullptr);
        }

        void OnFrame(const feature::Frame& f) override {
            const float dt = last > 0 ? float(std::min(f.now - last, 0.1)) : 0.0f;
            last = f.now;
            const bool focused = GameFocused();
            const bool k = focused && Down(kKeys[key].vk);
            if (k && !keyWas) flying ? Stop() : Start();
            keyWas = k;
            if (!flying) return;

            free_camera::Input in{};
            if (focused && !ImGui::GetIO().WantCaptureKeyboard) {
                in.forward = float(Down('W')) - float(Down('S'));
                in.right = float(Down('D')) - float(Down('A'));
                in.up = float(Down(VK_SPACE) || Down('E')) - float(Down(VK_CONTROL) || Down('Q'));
                in.fast = Down(VK_SHIFT);
                const float arrow = 90.0f * dt;
                in.lookYaw = arrow * (float(Down(VK_RIGHT)) - float(Down(VK_LEFT)));
                in.lookPitch = arrow * (float(Down(VK_UP)) - float(Down(VK_DOWN)));
            }
            // Right mouse held: mouse look; the cursor is pinned so it never hits the screen edge.
            const bool rmb = focused && Down(VK_RBUTTON) && !ImGui::GetIO().WantCaptureMouse;
            POINT c{};
            GetCursorPos(&c);
            if (rmb && looking) {
                in.lookYaw += sens * float(c.x - anchor.x);
                in.lookPitch -= sens * float(c.y - anchor.y);
                SetCursorPos(anchor.x, anchor.y);
            } else if (rmb) {
                anchor = c;
            }
            looking = rmb;

            pose = free_camera::Step(pose, in, dt, speed);
            const game::CamPose p{pose.x, pose.y, pose.z, pose.pitch, pose.yaw};
            game::SetFreeCam(&p);
        }

        void Off() override { Stop(); }

        void Menu() override {
            if (ImGui::Button(flying ? "Stop flying" : "Fly")) flying ? Stop() : Start();
            ImGui::SameLine(); ImGui::SetNextItemWidth(110);
            if (ImGui::BeginCombo("Key", kKeys[key].name)) {
                for (int i = 0; i < IM_ARRAYSIZE(kKeys); i++)
                    if (ImGui::Selectable(kKeys[i].name, i == key)) key = i;
                ImGui::EndCombo();
            }
            ImGui::SameLine();
            if (ImGui::Button("Log actors")) game::LogActors();
            if (const int logged = game::LoggedActors()) { ImGui::SameLine(); ImGui::TextDisabled("%d logged", logged); }
            ImGui::SetNextItemWidth(150);
            ImGui::SliderFloat("Speed", &speed, 100.0f, 4000.0f, "%.0f");
            ImGui::SetNextItemWidth(150);
            ImGui::SliderFloat("Mouse", &sens, 0.03f, 0.5f, "%.2f");
            ImGui::TextDisabled("WASD, Space/E up, Ctrl/Q down, Shift fast, hold RMB or arrows to look");
            if (flying) {
                ImGui::TextDisabled("pos %.0f %.0f %.0f  pitch %.0f yaw %.0f", pose.x, pose.y, pose.z, pose.pitch, pose.yaw);
                const int hits = game::FreeCamOverrides();
                ImGui::TextDisabled("camera %s, target %s, overrides %d", manager.c_str(), target.c_str(), hits);
                if (hits == 0) ImGui::TextColored(ImVec4(1, 0.6f, 0.2f, 1), "camera not taken over here (see dos-tool.log)");
            }
        }
    } g_freeCamera;
}

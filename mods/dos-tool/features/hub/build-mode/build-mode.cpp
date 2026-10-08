#include "feature.hpp"
#include "logger.hpp"
#include "imgui.h"
#include "../shared/aim.hpp"
#include "../shared/hub_state.hpp"
#include "../shared/hub_ui.hpp"
#include "../shared/layout.hpp"
#include "../shared/props.hpp"

#include <Windows.h>
#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

// Build mode for the hub: rearrange the tavern's props while you walk it (tavern hub, F7) or fly the free camera.
namespace {
    bool Down(int vk) { return (GetAsyncKeyState(vk) & 0x8000) != 0; }

    bool GameFocused() {
        DWORD pid = 0;
        GetWindowThreadProcessId(GetForegroundWindow(), &pid);
        return pid == GetCurrentProcessId();
    }

    struct BuildMode : feature::Feature {
        // Build mode (B while walking, also flying with F6): look at a prop (outlined like the NPCs), left click (or E
        // when not flying) picks it up; it rides in front of you - or where the free camera's view meets the floor -
        // at the height it had above the floor; R turns it 15°, Page Up/Down lifts it 5 cm, left click (E) puts it
        // down, right click (walking) / Backspace puts it back. Saved as PROP:<actor name>=x,y,z,yaw.
        bool build = false, buildWas = false, rotWas = false, pickWas = false, cancelWas = false, liftWas = false;
        bool bringWas = false, resetWas = false, carryNearHero = false;  // T: fetch it next to you; Delete: back home
        std::vector<props::Prop> nearProps;
        float scanX = 1e9f, scanY = 1e9f;
        std::string lookedProp, carried;
        float carryYaw = 0, carryAbove = 0, carryRadius = 0;
        hub_layout::Spot carriedFrom{};                // where it stood: cancel puts it back
        float lastX = 0, lastY = 0, lastZ = 0;         // where it rides now
        static constexpr float kCapsuleHalf = 90.0f;  // hub hero's capsule: centre → feet

        bool CarrySpot(const game::Hero& h, bool flying, float& x, float& y) {
            const float feet = h.z - kCapsuleHalf;
            if (flying && !carryNearHero) {  // where the free camera looks at the floor you stand on
                combat::View v{};
                return game::GetView(v) && hub_aim::RayToFloor(v.x, v.y, v.z, v.pitch, v.yaw, feet, 3000.0f, x, y);
            }
            const float r = hub_state::camYaw * hub_aim::kD2R, ahead = 80.0f + carryRadius;
            x = h.x + ahead * std::cos(r);
            y = h.y + ahead * std::sin(r);
            return true;
        }

        void Build(const game::Hero& h, bool keys, bool e, bool eWas) {
            const bool flying = game::CamOwner() == 1;
            const bool lmb = keys && Down(VK_LBUTTON), pick = (lmb && !pickWas) || (!flying && e && !eWas);
            pickWas = lmb;
            // right mouse turns the free camera's view: cancel there is Backspace only
            const bool cancelKey = keys && ((!flying && Down(VK_RBUTTON)) || Down(VK_BACK)), cancel = cancelKey && !cancelWas;
            cancelWas = cancelKey;
            combat::View v{};
            const bool haveView = game::GetView(v);
            // props around the eye that looks: the hero when walking, the free camera when flying
            const float ex = flying && haveView ? v.x : h.x, ey = flying && haveView ? v.y : h.y, ez = flying && haveView ? v.z : h.z;
            const float dx = ex - scanX, dy = ey - scanY;
            if (dx * dx + dy * dy > 200.0f * 200.0f) { nearProps = props::Near(ex, ey, ez, 2500.0f, {}); scanX = ex; scanY = ey; }
            const bool bringKey = keys && Down('T'), bring = bringKey && !bringWas;
            bringWas = bringKey;
            const bool resetKey = keys && Down(VK_DELETE), reset = resetKey && !resetWas;
            resetWas = resetKey;
            if (!carried.empty()) {
                const bool rot = keys && Down('R');
                if (rot && !rotWas) carryYaw = std::fmod(carryYaw + 15.0f, 360.0f);
                rotWas = rot;
                const bool up = keys && Down(VK_PRIOR), dn = keys && Down(VK_NEXT);
                if ((up || dn) && !liftWas) carryAbove += up ? 5.0f : -5.0f;
                liftWas = up || dn;
                float x = lastX, y = lastY;
                if (CarrySpot(h, flying, x, y)) { lastX = x; lastY = y; lastZ = h.z - kCapsuleHalf + carryAbove; }
                if (cancel) {
                    props::Move(carried, carriedFrom.x, carriedFrom.y, carriedFrom.z, carriedFrom.yaw, false);
                    props::Highlight(carried, false, 0);
                    logger::log("[tavern] put back " + carried);
                    carried.clear();
                    carryNearHero = false;
                    return;
                }
                props::Move(carried, lastX, lastY, lastZ, carryYaw, !pick);
                if (pick) {  // put it down where it rides, and remember
                    props::Highlight(carried, false, 0);
                    hub_layout::SetSpot(hub_layout::Spots(), {"PROP:" + carried, lastX, lastY, lastZ, carryYaw});
                    hub_layout::Save();
                    logger::log("[tavern] put down " + carried);
                    carried.clear();
                    carryNearHero = false;
                    scanX = 1e9f;  // positions changed: rescan
                }
                return;
            }
            std::string hit;
            const props::Prop* hp = nullptr;
            if (haveView) {  // what the crosshair is on: the nearest prop the view ray passes through
                std::vector<hub_aim::Sphere> ss;
                for (const props::Prop& p : nearProps) ss.push_back({p.x, p.y, p.cz, p.radius * 0.8f});
                const int i = hub_aim::RayPick(v.x, v.y, v.z, v.pitch, v.yaw, ss, 3000.0f);
                if (i >= 0) { hp = &nearProps[i]; hit = hp->name; }
            }
            if (hit != lookedProp) {
                if (!lookedProp.empty()) props::Highlight(lookedProp, false, 0);
                if (!hit.empty()) props::Highlight(hit, true, hub_ui::RimStencil());
                lookedProp = hit;
            }
            if (reset && hp) {  // back where the game had it, and forget our placement
                props::Reset(hp->name);
                props::Highlight(hp->name, false, 0);
                const std::string key = "PROP:" + hp->name;
                hub_layout::Spots().erase(std::remove_if(hub_layout::Spots().begin(), hub_layout::Spots().end(), [&](const hub_layout::Spot& s) { return s.name == key; }),
                             hub_layout::Spots().end());
                hub_layout::Save();
                logger::log("[tavern] reset " + hp->name);
                lookedProp.clear();
                scanX = 1e9f;
                return;
            }
            if ((pick || bring) && hp) {
                carried = hp->name;
                carriedFrom = {"", hp->x, hp->y, hp->z, hp->yaw};
                carryYaw = hp->yaw;
                // its height above the floor you both stand on; fetched from elsewhere: resting on your floor
                carryAbove = bring ? hp->base : hp->z - (h.z - kCapsuleHalf);
                carryRadius = hp->radius;
                carryNearHero = bring;
                lastX = hp->x; lastY = hp->y; lastZ = hp->z;
                lookedProp.clear();
                logger::log(std::string("[tavern] ") + (bring ? "fetched " : "picked up ") + carried);
            }
        }

        void EndBuild() {
            if (!lookedProp.empty()) props::Highlight(lookedProp, false, 0);
            if (!carried.empty()) props::Highlight(carried, false, 0);
            lookedProp.clear();
            carried.clear();  // ponytail: a prop still carried stays where it last was, without collision until reload
            build = hub_state::build = hub_state::carrying = false;
        }

        void ApplyProps() {
            for (const hub_layout::Spot& s : hub_layout::Spots())
                if (s.name.rfind("PROP:", 0) == 0) props::Move(s.name.substr(5), s.x, s.y, s.z, s.yaw, false);
        }

        std::string world;
        bool eWas = false;

        BuildMode() : Feature("Build mode", feature::Stage::Alpha) {}

        void OnFrame(const feature::Frame& f) override {
            if (f.snap.worldName != world) { world = f.snap.worldName; if (world == "Hub") ApplyProps(); }
            const bool keys = GameFocused() && !ImGui::GetIO().WantCaptureKeyboard, e = keys && Down('E');
            if (!hub_state::walking) { if (build) EndBuild(); eWas = e; return; }
            if (hub_state::screenOpen) { eWas = e; return; }  // a game screen has the input
            const bool b = keys && Down('B');
            if (b && !buildWas) { if (build) EndBuild(); else build = hub_state::build = true; }
            buildWas = b;
            if (build) { const game::Hero h = game::HubHero(); if (h.found) Build(h, keys, e, eWas); }
            hub_state::carrying = !carried.empty();
            eWas = e;
        }

        void Off() override { EndBuild(); props::Stop(); }
    } g_buildMode;
}

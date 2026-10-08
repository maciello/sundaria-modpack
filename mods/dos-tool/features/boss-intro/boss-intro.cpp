#include "feature.hpp"
#include "boss-intro.hpp"
#include "signals.hpp"
#include "logger.hpp"
#include "imgui.h"

#include <cstdio>
#include <string>

// Boss intro (#13): the game's own boss signals (signals.cpp) -> one intro per encounter: the camera swings onto the
// boss, orbits slowly and returns (#18). Spec: design-system.md § Boss intro camera.
// The camera is the core override (game::SetFreeCam, the free camera's path): nothing of the game's is written, so
// ending the override is the exact restore. Local to this client: co-op partners see nothing.
namespace {
    using namespace boss_intro;
    constexpr double kWaitForBoss = 5.0;  // s: an arena entered before the boss spawned

    struct Run {
        bool on = false, cam = false, haveYaw = false;
        double t0 = 0, asked = 0;
        std::uintptr_t fight = 0;
        float yaw = 0;
        game_side::Boss boss{};
    };

    struct BossIntro : feature::Feature {
        Detector detector;
        Run run;
        std::string last = "none yet";

        BossIntro() : Feature("Boss intro", feature::Stage::Alpha) { optIn = true; }  // new game-thread hook

        void Log(const char* fmt, double a) {
            char buf[160];
            std::snprintf(buf, sizeof(buf), fmt, a);
            logger::log(buf);
        }

        void End(const char* why, double now) {
            if (!run.on) return;
            if (run.cam) game::SetFreeCam(nullptr, 0);
            char buf[160];
            std::snprintf(buf, sizeof(buf), "[boss-intro] camera %s after %.1f s", why, run.cam ? now - run.t0 : 0.0);
            logger::log(buf);
            run = {};
        }

        bool Live(cam::Pose& out) {
            game::CamPose g;
            if (game::GameCamPose(g)) { out = {g.x, g.y, g.z, g.pitch, g.yaw, g.fovDelta}; return true; }
            combat::View v;
            if (run.cam || !game::GetView(v)) return false;  // overriding: the cached view is our own pose
            out = {v.x, v.y, v.z, v.pitch, v.yaw, v.fov};
            return true;
        }

        void Camera(const feature::Frame& f) {
            if (!run.on) return;
            game_side::Boss b;
            const bool haveBoss = game_side::BossOf(run.fight, b);
            if (haveBoss) run.boss = b;
            if (!run.cam) {  // not started: wait for the boss to exist
                if (!haveBoss) { if (f.now - run.asked > kWaitForBoss) End("skipped (no boss actor)", f.now); return; }
                run.t0 = f.now;
            }
            const float t = float(f.now - run.t0);
            if (t >= cam::kTotal) { End("done", f.now); return; }
            cam::Pose live;
            if (!Live(live)) { End("lost the game camera", f.now); return; }
            if (!run.haveYaw) { run.yaw = cam::YawTo(live.x, live.y, run.boss.x, run.boss.y); run.haveYaw = true; }
            const cam::Pose shot = cam::Shot(run.boss.x, run.boss.y, run.boss.z, run.boss.halfHeight, run.yaw + cam::Orbit(t), live.fov);
            const cam::Pose p = cam::Blend(live, shot, cam::Weight(t));
            const game::CamPose cp{p.x, p.y, p.z, p.pitch, p.yaw, p.fov - live.fov};
            game::SetFreeCam(&cp, 0);
            if (!run.cam) Log("[boss-intro] camera start, boss half height %.0f", run.boss.halfHeight);
            run.cam = true;

            ImDrawList* dl = ImGui::GetForegroundDrawList();
            const float bar = cam::Letterbox(t) * f.h;
            const ImU32 ink = style::Pack(style::color::kInk);
            dl->AddRectFilled(ImVec2(0, 0), ImVec2(f.w, bar), ink);
            dl->AddRectFilled(ImVec2(0, f.h - bar), ImVec2(f.w, f.h), ink);
        }

        void OnFrame(const feature::Frame& f) override {
            game_side::Listen(true);
            for (const game_side::Event& e : game_side::Take()) {
                const Verdict v = detector.On(e.signal, e.fight, f.now);
                char buf[400];
                std::snprintf(buf, sizeof(buf), "[boss-intro] %s %s \"%s\" \"%s\" -> %s", Name(e.signal),
                              e.fightClass.empty() ? "-" : e.fightClass.c_str(), e.name.c_str(), e.subtitle.c_str(), Name(v.kind));
                logger::log(buf);
                last = buf + 13;
                if (v.kind == Verdict::Start && !run.on) run = {true, false, false, 0, f.now, v.fight};
            }
            Camera(f);
        }

        void Off() override {
            End("off", 0);
            game_side::Listen(false);
            detector = {};
        }

        void Menu() override { ImGui::TextDisabled("last signal: %s", last.c_str()); }
    } g_feature;
}

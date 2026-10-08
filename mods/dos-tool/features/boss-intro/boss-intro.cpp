#include "feature.hpp"
#include "boss-intro.hpp"
#include "signals.hpp"
#include "logger.hpp"
#include "draw.hpp"
#include "imgui.h"

#include <cstdio>
#include <cfloat>
#include <string>
#include <unordered_map>

// Boss intro (#13): the game's own boss signals (signals.cpp) -> one intro per encounter: the camera swings onto the
// boss, orbits slowly and returns (#18) while the boss's name shows as a title (#19), unless the game shows its own
// boss splash. Spec: design-system.md § Boss intro camera; title constants in boss-intro.hpp.
// The camera is the core override (game::SetFreeCam, the free camera's path): nothing of the game's is written, so
// ending the override is the exact restore. Local to this client: co-op partners see nothing.
namespace {
    using namespace boss_intro;
    constexpr double kWaitForBoss = 5.0;  // s: an arena entered before the boss spawned

    struct Run {
        bool on = false, cam = false, haveYaw = false;
        double t0 = 0, asked = 0, splashAt = -1;  // splashAt: the game's own boss splash appeared (no title of ours)
        std::uintptr_t fight = 0;
        float yaw = 0;
        game_side::Boss boss{};
        std::string name, epithet;
    };
    struct Names { std::string name, epithet; };

    struct BossIntro : feature::Feature {
        Detector detector;
        Run run;
        std::unordered_map<std::uintptr_t, Names> names;  // per fight, from its signals. ponytail: never pruned (a few per dungeon)
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
            Title(f, dl, t);
        }

        void Title(const feature::Frame& f, ImDrawList* dl, float t) {
            using namespace style;
            const title::Look look = title::At(t);
            const float a = look.alpha * title::Fade(run.splashAt < 0 ? -1.0f : float(f.now - run.splashAt));
            if (a < 0.01f || run.name.empty() || !f.font) return;
            const float ui = type::Ui(f.h);
            const float np = (run.name.size() > title::kLongName ? type::kLg : type::kXl) * ui, ep = type::kMd * ui;
            const ImVec2 ns = f.font->CalcTextSizeA(np, FLT_MAX, 0, run.name.c_str());
            const float cx = f.w * 0.5f, top = f.h * title::kCentreY - ns.y * 0.5f + look.rise * ui;
            const ImU32 ink = Pack(color::kInk, stroke::kOutlineAlpha * title::kInkAlpha * a);
            const ImVec2 at(cx - ns.x * 0.5f, top);
            draw::OutlinedText(dl, f.font, np, at, 0, Pack(color::kTextSoft, stroke::kGlowAlpha * a),
                               stroke::Outline(np) * stroke::kGlowWidth, run.name.c_str());
            draw::OutlinedText(dl, f.font, np, at, Pack(color::kText, a), ink, stroke::Outline(np), run.name.c_str());
            // divider: fades to nothing at both ends, grows from the centre
            const float half = ns.x * title::kDividerWidth * 0.5f * look.divider, y = top - space::k3 * ui;
            const ImU32 on = Pack(color::kTextSoft, title::kDividerAlpha * a), off = Pack(color::kTextSoft, 0);
            const float dh = title::kDividerPx * ui;
            dl->AddRectFilledMultiColor(ImVec2(cx - half, y), ImVec2(cx, y + dh), off, on, on, off);
            dl->AddRectFilledMultiColor(ImVec2(cx, y), ImVec2(cx + half, y + dh), on, off, off, on);
            if (run.epithet.empty()) return;
            const ImVec2 es = f.font->CalcTextSizeA(ep, FLT_MAX, 0, run.epithet.c_str());
            draw::OutlinedText(dl, f.font, ep, ImVec2(cx - es.x * 0.5f, y - space::k3 * ui - es.y), Pack(color::kTextSoft, a), ink,
                               stroke::Outline(ep), run.epithet.c_str());
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
                if (!e.fightClass.empty()) names[e.fight] = {e.name.empty() ? title::FromClass(e.fightClass) : e.name, e.subtitle};
                if (v.kind == Verdict::Start && !run.on) {
                    run = {true, false, false, 0, f.now, -1, v.fight};
                    if (auto it = names.find(v.fight); it != names.end()) run.name = it->second.name, run.epithet = it->second.epithet;
                }
                if (e.signal == Signal::Splash && run.on && run.splashAt < 0) run.splashAt = f.now;
            }
            Camera(f);
        }

        void Off() override {
            End("off", 0);
            game_side::Listen(false);
            detector = {};
            names.clear();
        }

        void Menu() override { ImGui::TextDisabled("last signal: %s", last.c_str()); }
    } g_feature;
}

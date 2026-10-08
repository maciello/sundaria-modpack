#include "feature.hpp"
#include "boss-intro.hpp"
#include "signals.hpp"
#include "logger.hpp"
#include "draw.hpp"
#include "imgui.h"
#include "imgui_internal.h"  // MarkIniSettingsDirty

#include <Windows.h>
#include <set>
#include <cstdio>
#include <cfloat>
#include <string>
#include <unordered_map>

// Boss intro (#13): the game's own boss signals (signals.cpp) -> one intro per encounter: the camera swings onto the
// boss, orbits slowly and returns (#18) while the boss's name shows as a title (#19), unless the game shows its own
// boss splash. Spec: design-system.md § Boss intro camera; title constants in boss-intro.hpp.
// The camera is the core override (game::SetFreeCam, the free camera's path): nothing of the game's is written, so
// ending the override is the exact restore. Local to this client: co-op partners see nothing.
// The fight waits (#70): the host (or solo) freezes the boss, its partners and their AI for the intro, restored on every
// end (done, skip, Off, map change via "no local pawn"). A co-op client simulates no AI, so it freezes nothing and sees
// the boss act during its own intro; the host's intro freezes the boss for everyone, at most kTotal + kWaitForBoss.
// Skip (#20): the chosen key or Escape (the game's menu) ends camera and title at once; so do losing the pawn, being
// downed and the game camera stopping. Never blocks input or other players.
namespace {
    using namespace boss_intro;
    constexpr double kWaitForBoss = 5.0;  // s: an arena entered before the boss spawned
    constexpr double kCamGrace = 0.25;    // s without a fresh game pose before it is logged as stale
    struct Key { int vk; const char* name; };
    constexpr Key kKeys[] = {{VK_SPACE, "Space"}, {VK_RETURN, "Enter"}, {VK_BACK, "Backspace"}, {'X', "X"}, {VK_F5, "F5"}};

    bool GameFocused() {
        DWORD pid = 0;
        GetWindowThreadProcessId(GetForegroundWindow(), &pid);
        return pid == GetCurrentProcessId();
    }
    bool Down(int vk) { return (GetAsyncKeyState(vk) & 0x8000) != 0; }

    struct Run {
        bool on = false, cam = false, haveYaw = false;
        double t0 = 0, asked = 0, splashAt = -1;  // splashAt: the game's own boss splash appeared (no title of ours)
        std::uintptr_t fight = 0;
        float yaw = 0;
        game_side::Boss boss{};
        cam::Pose live{};
        double liveAt = -1;  // last fresh game pose
        bool staleLogged = false;
        std::string name, epithet;
    };
    struct Names { std::string name, epithet; };

    struct BossIntro : feature::Feature {
        Detector detector;
        Run run;
        std::unordered_map<std::uintptr_t, Names> names;  // per fight, from its signals. ponytail: never pruned (a few per dungeon)
        std::string last = "none yet";
        int key = 0;              // index into kKeys
        bool oncePerBoss = false; // per game session
        bool keyWas = true;       // a key held when the intro starts is not a skip
        std::set<std::string> shown;  // boss names already introduced this session

        BossIntro() : Feature("Boss intro", feature::Stage::Alpha) { optIn = true; }  // new game-thread hook

        void Log(const char* fmt, double a) {
            char buf[160];
            std::snprintf(buf, sizeof(buf), fmt, a);
            logger::log(buf);
        }

        void End(const char* why, double now) {
            if (!run.on) return;
            game::SetFreeCam(nullptr, 0);  // also clears a slot taken while outranked
            if (const int n = game_side::Resume()) Log("[boss-intro] restored %.0f actors", n);
            char buf[160];
            std::snprintf(buf, sizeof(buf), "[boss-intro] camera %s after %.1f s (camera overrides so far %d)", why,
                          run.cam ? now - run.t0 : 0.0, game::FreeCamOverrides());
            logger::log(buf);
            run = {};
        }

        // The game's own camera pose. A frame without a fresh one reuses the last (#72: the intro ended at once when
        // the first frames after the override, a hitch, or a camera Blueprint that computes no pose gave none).
        bool Live(cam::Pose& out, double now) {
            game::CamPose g;
            combat::View v;
            if (game::GameCamPose(g)) run.live = {g.x, g.y, g.z, g.pitch, g.yaw, g.fovDelta}, run.liveAt = now;
            else if (!run.cam && game::GetView(v)) run.live = {v.x, v.y, v.z, v.pitch, v.yaw, v.fov}, run.liveAt = now;  // overriding: the view is ours
            else if (run.liveAt >= 0 && now - run.liveAt > kCamGrace && !run.staleLogged) {
                run.staleLogged = true;
                Log("[boss-intro] game camera pose stale for %.2f s: holding the last one", now - run.liveAt);
            }
            out = run.live;
            return run.liveAt >= 0;
        }

        const char* Interrupted(const feature::Frame& f) {
            const bool k = GameFocused() && (Down(kKeys[key].vk) || Down(VK_ESCAPE));
            const bool pressed = k && !keyWas;
            keyWas = k;
            if (pressed) return "skipped";
            const std::uintptr_t pawn = game_side::LocalPawn();
            if (!pawn) return "ended (no local pawn)";
            for (const combat::Sample& s : f.chars)
                if (s.id == pawn && s.health <= 0) return "ended (downed)";
            return nullptr;
        }

        void Camera(const feature::Frame& f) {
            if (!run.on) return;
            if (const char* why = Interrupted(f)) { End(why, f.now); return; }
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
            if (!Live(live, f.now)) { End("lost the game camera", f.now); return; }
            if (!run.haveYaw) { run.yaw = cam::YawTo(live.x, live.y, run.boss.x, run.boss.y); run.haveYaw = true; }
            const cam::Pose shot = cam::Shot(run.boss.x, run.boss.y, run.boss.z, run.boss.halfHeight, run.yaw + cam::Orbit(t), live.fov);
            const cam::Pose p = cam::Blend(live, shot, cam::Weight(t));
            const game::CamPose cp{p.x, p.y, p.z, p.pitch, p.yaw, p.fov - live.fov};
            const bool shown = game::SetFreeCam(&cp, 0);
            if (!run.cam && !shown) {  // no window focus yet: the sequence has not started
                if (f.now - run.asked > kWaitForBoss) End("skipped (camera override unavailable)", f.now);
                return;
            }
            if (!run.cam) Log("[boss-intro] camera start, boss half height %.0f", run.boss.halfHeight);
            run.cam = true;
            if (const int n = game_side::Pause(run.fight)) Log("[boss-intro] paused %.0f actors", n);  // partners may spawn later

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
            if (detector.engaged && !game_side::Alive(detector.engaged)) detector.engaged = 0;  // left the map mid-fight
            for (const game_side::Event& e : game_side::Take()) {
                const Verdict v = detector.On(e.signal, e.fight, f.now);
                char buf[400];
                std::snprintf(buf, sizeof(buf), "[boss-intro] %s %s \"%s\" \"%s\" -> %s", Name(e.signal),
                              e.fightClass.empty() ? "-" : e.fightClass.c_str(), e.name.c_str(), e.subtitle.c_str(), Name(v.kind));
                logger::log(buf);
                last = buf + 13;
                if (!e.fightClass.empty()) {
                    const std::string n = e.name.empty() ? title::FromClass(e.fightClass) : e.name;
                    names[e.fight] = {n, title::Epithet(e.subtitle, n)};
                }
                if (v.kind == Verdict::Start && !run.on) {
                    run = {true, false, false, 0, f.now, -1, v.fight};
                    if (auto it = names.find(v.fight); it != names.end()) run.name = it->second.name, run.epithet = it->second.epithet;
                    keyWas = true;
                    if (oncePerBoss && !run.name.empty() && !shown.insert(run.name).second) {
                        logger::log("[boss-intro] " + run.name + " already introduced this session");
                        run = {};
                    }
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

        void Menu() override {
            ImGui::SetNextItemWidth(110);
            if (ImGui::BeginCombo("Skip key", kKeys[key].name)) {
                for (int i = 0; i < IM_ARRAYSIZE(kKeys); i++)
                    if (ImGui::Selectable(kKeys[i].name, i == key)) { key = i; ImGui::MarkIniSettingsDirty(); }
                ImGui::EndCombo();
            }
            ImGui::SameLine(); ImGui::TextDisabled("(Escape too)");
            if (ImGui::Checkbox("Once per boss per session", &oncePerBoss)) ImGui::MarkIniSettingsDirty();
            ImGui::TextDisabled("last signal: %s", last.c_str());
        }

        void Load(const char* k, const char* v) override {
            const std::string s = k;
            if (s == "key") key = std::clamp(std::atoi(v), 0, int(IM_ARRAYSIZE(kKeys)) - 1);
            if (s == "once") oncePerBoss = std::atoi(v) != 0;
        }
        void Save(std::vector<std::pair<std::string, std::string>>& out) override {
            out.push_back({"key", std::to_string(key)});
            out.push_back({"once", oncePerBoss ? "1" : "0"});
        }
    } g_feature;
}

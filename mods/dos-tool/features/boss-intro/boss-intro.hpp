#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string>
#include <vector>
#include "style.hpp"

// SDK-free logic for the boss intro (#13). Spec: references/design-system.md § Boss intro camera, § Boss name card.
namespace boss_intro {
    // The game's own boss signals, each a ProcessEvent call (signals.cpp). Fight signals carry the fight actor.
    enum class Signal : std::uint8_t {
        FightBegin,    // a boss fight actor's ReceiveBeginPlay (map load; also a new instance after travel)
        ArenaEnter,    // the local pawn begins overlapping the fight's ArenaTrigger
        SpawnTrigger,  // the local pawn begins overlapping the fight's MasterSpawnTrigger (bosses spawn)
        CombatStart,   // MulticastNotifyCombatStart (NetMulticast: reaches every client)
        Finished,      // MulticastNotifyFinished (won or wiped)
        Splash,        // the game's boss splash widget (WidgetBossSplashScreen_C) constructs
        Lens,          // the boss announcement lens effect (BP_LensEffect_bossAnnouncement_C) begins play
    };
    inline const char* Name(Signal s) {
        constexpr const char* n[] = {"fight-begin", "arena-enter", "spawn-trigger", "combat-start", "finished", "splash", "lens"};
        return n[int(s)];
    }

    // Splash and lens carry no fight: within this many seconds of an intro they belong to it.
    constexpr double kSameEncounter = 15.0;

    struct Verdict {
        enum Kind : std::uint8_t { Start, Same, Rearm } kind;
        std::uintptr_t fight;  // Start: the fight to introduce (0 = unknown)
    };
    inline const char* Name(Verdict::Kind k) { return k == Verdict::Start ? "intro start" : k == Verdict::Same ? "same encounter" : "re-armed"; }

    // One intro per encounter: the first boss signal of a fight starts it; the fight's end (won or wiped) or a
    // new instance of it re-arms. A normal elite never sends any of these signals.
    struct Detector {
        std::vector<std::uintptr_t> fired;
        std::uintptr_t lastFight = 0;
        double lastStart = -1e9;

        bool Fired(std::uintptr_t f) const { return std::find(fired.begin(), fired.end(), f) != fired.end(); }
        void Rearm(std::uintptr_t f) { fired.erase(std::remove(fired.begin(), fired.end(), f), fired.end()); }

        Verdict On(Signal s, std::uintptr_t fight, double now) {
            switch (s) {
                case Signal::FightBegin:
                case Signal::Finished:
                    Rearm(fight);
                    if (s == Signal::FightBegin) lastFight = fight;
                    return {Verdict::Rearm, fight};
                case Signal::Splash:
                case Signal::Lens:
                    if (now - lastStart < kSameEncounter || (lastFight && Fired(lastFight))) return {Verdict::Same, lastFight};
                    fight = lastFight;
                    break;
                default:
                    lastFight = fight;
                    if (Fired(fight)) return {Verdict::Same, fight};
            }
            if (fight) fired.push_back(fight);
            lastStart = now;
            return {Verdict::Start, fight};
        }
    };

    // Camera move (#18): live camera -> framing shot -> slow orbit -> back to the live camera, re-read every frame.
    namespace cam {
        constexpr float kApproach = 1.2f, kReturn = 0.8f;   // s, InOutCubic
        constexpr float kHold = style::motion::kCardHold;  // the title plays meanwhile
        constexpr float kOrbitDegPerSec = 4.0f;            // linear, during the hold
        constexpr float kLookUp = 10.0f;                   // camera 10° below the boss's eyes, looking up
        constexpr float kDistPerHeight = 2.5f;             // × capsule height
        constexpr float kMinDist = 300.0f, kMaxDist = 3000.0f;  // cm
        constexpr float kEye = 0.8f;                       // eye height above the capsule centre, × half height
        constexpr float kFovDelta = -10.0f;                // vs gameplay
        constexpr float kLetterbox = 0.1f, kLetterboxIn = 0.4f;  // × screen height, s
        constexpr float kTotal = kApproach + kHold + kReturn;
        constexpr float kD2R = 3.14159265f / 180.0f;

        struct Pose { float x, y, z, pitch, yaw, fov; };  // fov horizontal, degrees

        // Shot share of the pose at t seconds into the sequence: 0 = live camera, 1 = framing shot.
        inline float Weight(float t) {
            using style::ease::Apply; using C = style::ease::Curve;
            if (t < kApproach) return Apply(C::InOutCubic, t / kApproach);
            if (t < kApproach + kHold) return 1.0f;
            return 1.0f - Apply(C::InOutCubic, (t - kApproach - kHold) / kReturn);
        }
        inline float Orbit(float t) { return kOrbitDegPerSec * std::clamp(t - kApproach, 0.0f, kHold); }
        inline float Letterbox(float t) {  // bar height, × screen height
            using style::ease::Apply; using C = style::ease::Curve;
            return kLetterbox * Apply(C::InOutCubic, std::min(t, kTotal - t) / kLetterboxIn);
        }

        inline float Wrap(float deg) { return std::remainder(deg, 360.0f); }  // -180..180
        inline float LerpAngle(float a, float b, float w) { return a + Wrap(b - a) * w; }
        inline float Lerp(float a, float b, float w) { return a + (b - a) * w; }

        // Framing shot of a boss (capsule centre bx,by,bz, half height hh) seen along `yaw` (degrees, + orbit):
        // eyes on the right third, camera below eye level looking up kLookUp, FOV = live + kFovDelta.
        inline Pose Shot(float bx, float by, float bz, float hh, float yaw, float liveFov) {
            const float d = std::clamp(kDistPerHeight * 2.0f * hh, kMinDist, kMaxDist);
            const float ez = bz + kEye * hh;
            const float p = kLookUp * kD2R, y = yaw * kD2R;
            const float fov = std::clamp(liveFov + kFovDelta, 20.0f, 170.0f);
            // camera-space direction to the eyes = (fwd 1, right r, up 0), r = screen x at 2/3 w; solve the camera's
            // pitch/yaw so that direction has elevation kLookUp along `yaw` in the world.
            const float r = std::tan(fov * 0.5f * kD2R) / 3.0f;
            const float pitch = std::asin(std::sin(p) * std::sqrt(1 + r * r));
            const float camYaw = yaw - std::atan2(r, std::cos(pitch)) / kD2R;
            return {bx - d * std::cos(p) * std::cos(y), by - d * std::cos(p) * std::sin(y), ez - d * std::sin(p),
                    pitch / kD2R, Wrap(camYaw), fov};
        }
        inline Pose Blend(const Pose& live, const Pose& shot, float w) {
            return {Lerp(live.x, shot.x, w), Lerp(live.y, shot.y, w), Lerp(live.z, shot.z, w),
                    LerpAngle(live.pitch, shot.pitch, w), Wrap(LerpAngle(live.yaw, shot.yaw, w)), Lerp(live.fov, shot.fov, w)};
        }
        // Direction from the live camera to the boss: the shot looks the way the player already does.
        inline float YawTo(float fromX, float fromY, float bx, float by) { return std::atan2(by - fromY, bx - fromX) / kD2R; }
    }

    // Boss title (#19, Genshin style): centred name + optional epithet over a thin divider, soft fade, during the hold.
    // The game's own boss splash wins: while it shows, no title of ours.
    namespace title {
        constexpr float kCentreY = 0.76f;      // × screen height, above the bottom letterbox
        constexpr float kRise = 12.0f;         // px at 1080p, fade-in rise
        constexpr float kDividerPx = 1.5f;     // px at 1080p
        constexpr float kDividerAlpha = 0.7f;
        constexpr float kDividerWidth = 1.15f; // × name width
        constexpr float kInkAlpha = 0.6f;      // × stroke::kOutlineAlpha: a light outline, not a heavy one
        constexpr int kLongName = 18;          // chars: longer names use type::kLg
        constexpr float kSkip = 0.1f;          // s: everything out on skip / when the game's splash appears
        constexpr float kIn = cam::kApproach, kOut = cam::kApproach + cam::kHold;  // shown during the hold

        struct Look { float alpha, rise, divider; };  // rise in px at 1080p, divider = share of full width
        inline Look At(float t) {
            using style::ease::Apply; using namespace style::motion;
            if (t < kIn || t >= kOut) return {0, 0, 0};
            const float in = Apply(kCardIn.curve, (t - kIn) / kCardIn.dur);
            const float out = Apply(kCardOut.curve, (t - (kOut - kCardOut.dur)) / kCardOut.dur);
            return {in * (1 - out), kRise * (1 - in), in};
        }
        // 1 → 0 over kSkip after `since` seconds (skip pressed, game splash shown); 1 while since < 0 (not happened).
        inline float Fade(float since) { return since < 0 ? 1.0f : std::max(0.0f, 1 - since / kSkip); }

        // Fallback name from the fight class: BP_BossFight_CricTheThief_2nd_C -> "Cric The Thief 2nd".
        inline std::string FromClass(std::string s) {
            const std::string pre = "BP_BossFight_", suf = "_C";
            if (s.rfind(pre, 0) == 0) s.erase(0, pre.size());
            else if (s.rfind("BP_", 0) == 0) s.erase(0, 3);
            if (s.size() > suf.size() && s.compare(s.size() - suf.size(), suf.size(), suf) == 0) s.erase(s.size() - suf.size());
            std::string out;
            for (size_t i = 0; i < s.size(); i++) {
                const char c = s[i] == '_' ? ' ' : s[i];
                if (c >= 'A' && c <= 'Z' && i && s[i - 1] >= 'a' && s[i - 1] <= 'z') out += ' ';
                if (c != ' ' || (!out.empty() && out.back() != ' ')) out += c;
            }
            return out;
        }
    }
}

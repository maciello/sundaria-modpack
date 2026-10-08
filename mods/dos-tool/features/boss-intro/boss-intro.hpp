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
        enum Kind : std::uint8_t { Start, Same, Rearm, Noted, Busy } kind;
        std::uintptr_t fight;  // Start: the fight to introduce (0 = unknown)
    };
    inline const char* Name(Verdict::Kind k) {
        constexpr const char* n[] = {"intro start", "same encounter", "re-armed", "noted (not a start)", "another fight engaged"};
        return n[k];
    }

    // One intro per encounter, only when THIS fight starts: the local pawn enters its arena or its combat starts.
    // A spawn trigger never starts one (#74: a fight's MasterSpawnTrigger volume can reach into the arena before it,
    // so it fired for the next boss while the current one was on). No intro while another fight is engaged
    // (combat started, not finished). The fight's end or a new instance of it re-arms.
    struct Detector {
        std::vector<std::uintptr_t> fired;
        std::uintptr_t lastFight = 0, engaged = 0;
        double lastStart = -1e9;

        bool Fired(std::uintptr_t f) const { return std::find(fired.begin(), fired.end(), f) != fired.end(); }
        void Rearm(std::uintptr_t f) { fired.erase(std::remove(fired.begin(), fired.end(), f), fired.end()); }
        bool Busy(std::uintptr_t f) const { return engaged && engaged != f; }

        Verdict On(Signal s, std::uintptr_t fight, double now) {
            switch (s) {
                case Signal::FightBegin:
                case Signal::Finished:
                    Rearm(fight);
                    if (engaged == fight) engaged = 0;
                    if (s == Signal::FightBegin) lastFight = fight;
                    return {Verdict::Rearm, fight};
                case Signal::SpawnTrigger:
                    return {Verdict::Noted, fight};
                case Signal::Splash:
                case Signal::Lens:
                    if (now - lastStart < kSameEncounter || (lastFight && Fired(lastFight))) return {Verdict::Same, lastFight};
                    if (Busy(lastFight)) return {Verdict::Busy, lastFight};
                    fight = lastFight;
                    break;
                default:  // ArenaEnter, CombatStart
                    if (Busy(fight)) return {Verdict::Busy, fight};
                    if (s == Signal::CombatStart) engaged = fight;
                    lastFight = fight;
                    if (Fired(fight)) return {Verdict::Same, fight};
            }
            if (fight) fired.push_back(fight);
            lastStart = now;
            return {Verdict::Start, fight};
        }
    };

    // The fight waits while its intro plays (#70): each boss/partner actor and its AI controller this machine simulates
    // gets CustomTimeDilation 0 (its tick and its components' ticks, movement, animation, behaviour tree, run with
    // delta 0), captured once; restore puts back the captured value unless the game has changed it since (it wins).
    namespace pause {
        template<class K> struct Held { K key; float was; };
        template<class K> bool Hold(std::vector<Held<K>>& held, const K& k, float& dilation) {
            for (const Held<K>& h : held)
                if (h.key == k) return false;
            held.push_back({k, dilation});
            dilation = 0;
            return true;
        }
        template<class K> bool Release(const Held<K>& h, float& dilation) {
            if (dilation != 0) return false;
            dilation = h.was;
            return true;
        }
    }

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
        // reach < 1 pulls the camera in along the same line (a wall): framing and angles stay.
        inline Pose Shot(float bx, float by, float bz, float hh, float yaw, float liveFov, float reach = 1.0f) {
            const float d = std::clamp(kDistPerHeight * 2.0f * hh, kMinDist, kMaxDist) * reach;
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
        // Walls (#71), like a spring arm: the game thread sweeps a kProbe sphere from the boss's eyes to the full-reach
        // shot every camera update; reach follows the clear share, in at once, back out at kReachOut per second.
        constexpr float kProbe = 20.0f;    // cm
        constexpr float kReachOut = 1.0f;  // share per s
        inline float Follow(float reach, float clear, float dt) { return clear <= reach ? clear : std::min(clear, reach + kReachOut * dt); }
        // A blocked shot turns the orbit the other way, without a jump: yaw + dir * Orbit(t) is the same before and after.
        inline void Flip(float& yaw, float& dir, float t) { yaw += 2 * dir * Orbit(t); dir = -dir; }

        inline float YawTo(float fromX, float fromY, float bx, float by) { return std::atan2(by - fromY, bx - fromX) / kD2R; }

        // Face (#99): the camera stands in front of the boss, `side` degrees off its forward (actor yaw), never behind.
        constexpr float kFaceArc = 60.0f;      // deg: widest side angle; the orbit adds at most Orbit(kTotal) (≈ 10°) after a flip
        constexpr float kClearEnough = 0.9f;   // share of the framing distance: the player's side is kept when this clear
        constexpr float kFaceTry[] = {0.0f, -30.0f, 30.0f, -kFaceArc, kFaceArc};  // fallbacks when the player's side is blocked
        constexpr int kFaceCandidates = 1 + int(sizeof(kFaceTry) / sizeof(kFaceTry[0]));
        // Preferred side: where the live camera stands around the boss, clamped into the front arc.
        inline float FaceSide(float bossYaw, float fromX, float fromY, float bx, float by) {
            return std::clamp(Wrap(YawTo(bx, by, fromX, fromY) - bossYaw), -kFaceArc, kFaceArc);
        }
        inline float Side(int i, float preferred) { return i == 0 ? preferred : kFaceTry[i - 1]; }
        // Shot yaw (the view direction) for a camera standing `side` degrees off the boss's forward: looking back at it.
        inline float FaceYaw(float bossYaw, float side) { return Wrap(bossYaw + side + 180.0f); }
        // clear[i] = swept share of candidate Side(i): the preferred side if clear enough, else the clearest (earlier wins ties).
        inline int Best(const float* clear, int n) {
            if (clear[0] >= kClearEnough) return 0;
            int b = 0;
            for (int i = 1; i < n; i++)
                if (clear[i] > clear[b]) b = i;
            return b;
        }
        // The orbit turns towards the boss's front, so it never carries the camera further round its side.
        inline float OrbitDir(float side) { return side > 0 ? -1.0f : 1.0f; }
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

        // Epithet from the fight's FightStartedMessage, a format template (#73): {FightName} becomes the name; any
        // other {...} left means no epithet (never show a raw template).
        inline std::string Epithet(std::string t, const std::string& name) {
            const std::string key = "{FightName}";
            for (size_t i; (i = t.find(key)) != std::string::npos;) t.replace(i, key.size(), name);
            return t.find('{') == std::string::npos ? t : "";
        }

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

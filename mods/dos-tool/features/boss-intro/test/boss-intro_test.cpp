// just test
#include "boss-intro.hpp"
#include "combat.hpp"
#include <cassert>
#include <cstdio>

using namespace boss_intro;

int main() {
    {   // first fight signal starts the intro once; later signals of the same fight do not
        Detector d;
        assert(d.On(Signal::FightBegin, 1, 0).kind == Verdict::Rearm);
        Verdict v = d.On(Signal::ArenaEnter, 1, 10);
        assert(v.kind == Verdict::Start && v.fight == 1);
        assert(d.On(Signal::SpawnTrigger, 1, 11).kind == Verdict::Noted);
        assert(d.On(Signal::CombatStart, 1, 12).kind == Verdict::Same);
        assert(d.On(Signal::Splash, 0, 12.5).kind == Verdict::Same);  // the game's splash belongs to this intro
        assert(d.On(Signal::Lens, 0, 40).kind == Verdict::Same);       // later, but the fight already had its intro
        // wipe: the fight ends and restarts -> fires again
        assert(d.On(Signal::Finished, 1, 60).kind == Verdict::Rearm);
        assert(d.On(Signal::CombatStart, 1, 90).kind == Verdict::Start);
    }
    {   // #74: a spawn trigger never starts an intro; fight B's signals while fight A is engaged start nothing
        Detector d;
        assert(d.On(Signal::SpawnTrigger, 1, 0).kind == Verdict::Noted);   // Countess: spawn trigger before the arena
        assert(d.On(Signal::ArenaEnter, 1, 3).kind == Verdict::Start);
        assert(d.On(Signal::CombatStart, 1, 5).kind == Verdict::Same);     // A engaged
        assert(d.On(Signal::SpawnTrigger, 2, 6).kind == Verdict::Noted);   // Crypt Lord's volume reaches into Verix's arena
        assert(d.On(Signal::ArenaEnter, 2, 7).kind == Verdict::Busy);
        assert(d.On(Signal::CombatStart, 2, 8).kind == Verdict::Busy);
        assert(d.On(Signal::Finished, 1, 60).kind == Verdict::Rearm);     // A won
        assert(d.On(Signal::ArenaEnter, 2, 90).kind == Verdict::Start);   // now B may start
    }
    {   // two fights in one dungeon: each gets its own intro
        Detector d;
        assert(d.On(Signal::CombatStart, 1, 0).kind == Verdict::Start);
        d.On(Signal::Finished, 1, 50);
        assert(d.On(Signal::CombatStart, 2, 100).kind == Verdict::Start);
        assert(d.On(Signal::CombatStart, 1, 101).kind == Verdict::Busy);  // 2 is engaged
    }
    {   // splash alone (no fight signal seen, e.g. after a hot reload) starts an intro for the last fight known
        Detector d;
        d.On(Signal::FightBegin, 7, 0);
        Verdict v = d.On(Signal::Splash, 0, 5);
        assert(v.kind == Verdict::Start && v.fight == 7);
        assert(d.On(Signal::CombatStart, 7, 6).kind == Verdict::Same);
        // no fight known at all: still one intro, then the window holds
        Detector e;
        assert(e.On(Signal::Lens, 0, 1).kind == Verdict::Start);
        assert(e.On(Signal::Splash, 0, 2).kind == Verdict::Same);
        assert(e.On(Signal::Splash, 0, 1 + kSameEncounter + 1).kind == Verdict::Start);
    }
    {   // map travel: a new fight instance at a reused address is re-armed by its BeginPlay
        Detector d;
        d.On(Signal::CombatStart, 3, 0);
        d.On(Signal::FightBegin, 3, 500);
        assert(d.On(Signal::ArenaEnter, 3, 510).kind == Verdict::Start);
    }
    {   // #70: pause holds each actor once, restores its own value, and leaves a value the game changed meanwhile
        std::vector<pause::Held<int>> held;
        float boss = 1.0f, add = 0.5f;
        assert(pause::Hold(held, 1, boss) && boss == 0);
        assert(!pause::Hold(held, 1, boss) && held.size() == 1);  // every frame: captured once, never as 0
        assert(pause::Hold(held, 2, add) && add == 0);
        add = 0.3f;                                                // the game slowed it meanwhile
        assert(pause::Release(held[0], boss) && boss == 1.0f);
        assert(!pause::Release(held[1], add) && add == 0.3f);
    }
    {   // #100: adds near the player or the boss are candidates; players, the dead and the far are not
        const float r = pause::kAddRadius;
        auto ch = [](std::uintptr_t id, float x, float hp, bool player) { combat::Sample s{}; s.id = id, s.x = x, s.health = hp, s.isPlayer = player; return s; };
        const std::vector<combat::Sample> chars = {
            ch(1, 0, 100, true),            // me, at 0
            ch(2, r - 1, 50, false),        // near me
            ch(3, 10000 + r - 1, 50, false),// near the boss (at 10000)
            ch(4, 5000, 50, false),         // between, out of both radii
            ch(5, 10, 0, false),            // dead
            ch(6, 20, 100, true),           // a co-op partner
            ch(7, 10000, 900, false),       // the boss itself (the game thread holds it once)
        };
        auto ids = [](const std::vector<combat::Sample>& v) { std::vector<std::uintptr_t> o; for (const auto& s : v) o.push_back(s.id); return o; };
        assert((ids(pause::Near(chars, 1, 10000, 0, 0)) == std::vector<std::uintptr_t>{2, 3, 7}));
        assert((ids(pause::Near(chars, 0, 10000, 0, 0)) == std::vector<std::uintptr_t>{3, 7}));  // no local pawn: around the boss only
        assert(pause::Near({}, 1, 0, 0, 0).empty());
    }
    {   // camera: no pop at either end, full shot in the hold, orbit only in the hold
        using namespace cam;
        auto near = [](float a, float b, float e = 1e-3f) { return std::fabs(a - b) < e; };
        assert(near(Weight(0), 0) && near(Weight(kApproach), 1) && near(Weight(kApproach + kHold), 1) && near(Weight(kTotal), 0));
        assert(near(Orbit(kApproach), 0) && near(Orbit(kTotal), kOrbitDegPerSec * kHold));
        assert(near(Letterbox(0), 0) && near(Letterbox(kTotal / 2), kLetterbox) && near(Letterbox(kTotal), 0));
        assert(kTotal > 4.5f && kTotal < 4.7f);  // ≈ 4.6 s (spec)
        // angles take the short way round
        assert(near(LerpAngle(170, -170, 0.5f), 180) || near(LerpAngle(170, -170, 0.5f), -180));
        const Pose live{0, 0, 200, -15, 170, 90};
        Pose shotEnd = Blend(live, Shot(1000, 0, 100, 100, 0, 90), 0);
        assert(near(shotEnd.x, live.x) && near(shotEnd.yaw, live.yaw) && near(shotEnd.fov, live.fov));
        // framing: boss eyes on the right third, mid height; distance 2.5 x capsule height
        for (float yaw : {0.0f, 77.0f, -135.0f}) {
            const float hh = 120;
            const Pose s = Shot(500, -300, 50, hh, yaw, 90);
            assert(near(s.fov, 80));
            const float ez = 50 + kEye * hh;
            const float dist = std::sqrt((s.x - 500) * (s.x - 500) + (s.y + 300) * (s.y + 300) + (s.z - ez) * (s.z - ez));
            assert(near(dist, kDistPerHeight * 2 * hh, 0.5f));
            float sx, sy;
            assert(combat::Project({s.x, s.y, s.z, s.pitch, s.yaw, 0, s.fov}, 500, -300, ez, 1920, 1080, sx, sy));
            assert(near(sx, 1280, 1) && near(sy, 540, 1));
        }
        {   // #71: a wall pulls the camera in along the same line; the orbit reverses without a jump
            const Pose full = Shot(500, -300, 50, 120, 30, 90), half = Shot(500, -300, 50, 120, 30, 90, 0.5f);
            const float ez = 50 + kEye * 120;
            assert(near(half.x - 500, (full.x - 500) / 2, 0.5f) && near(half.z - ez, (full.z - ez) / 2, 0.5f));
            assert(near(half.pitch, full.pitch) && near(half.yaw, full.yaw));
            assert(near(Follow(1, 0.4f, 0.016f), 0.4f));                         // in at once
            assert(near(Follow(0.4f, 1, 0.1f), 0.4f + kReachOut * 0.1f));        // out eased
            assert(near(Follow(0.95f, 1, 0.1f), 1));
            float yaw = 10, dir = 1;
            const float t = kApproach + 2, before = yaw + dir * Orbit(t);
            Flip(yaw, dir, t);
            assert(dir == -1 && near(yaw + dir * Orbit(t), before) && yaw + dir * Orbit(t + 1) < before);
        }
        // small bosses are not shot from inside their capsule
        const Pose tiny = Shot(0, 0, 0, 10, 0, 90);
        assert(std::sqrt(tiny.x * tiny.x + tiny.y * tiny.y) > kMinDist * 0.9f);
        assert(near(YawTo(0, 0, 0, 100), 90));
    }
    {   // #99 face: the camera stands in front of the boss, on the player's side when that is in front, never behind
        using namespace cam;
        auto near = [](float a, float b) { return std::fabs(a - b) < 0.01f; };
        // boss at origin facing +X (yaw 0); a camera standing at angle a around it, 1000 cm out
        auto side = [](float bossYaw, float a) { return FaceSide(bossYaw, 1000 * std::cos(a * kD2R), 1000 * std::sin(a * kD2R), 0, 0); };
        assert(near(side(0, 0), 0) && near(side(0, 40), 40) && near(side(0, -40), -40));  // in front: kept
        assert(near(side(0, 180), kFaceArc) || near(side(0, 180), -kFaceArc));            // straight behind: an arc edge
        assert(near(side(0, 150), kFaceArc) && near(side(0, -150), -kFaceArc));            // behind-left stays left
        assert(near(side(90, 90 + 20), 20) && near(side(-170, 170), -20));                 // wraps
        // the shot looks back at the boss: the camera position is on the `side` of its forward
        for (float bossYaw : {0.0f, 90.0f, -135.0f})
            for (float s : {-kFaceArc, 0.0f, 25.0f, kFaceArc}) {
                const Pose p = Shot(0, 0, 0, 100, FaceYaw(bossYaw, s), 90);
                assert(std::fabs(Wrap(YawTo(0, 0, p.x, p.y) - bossYaw - s)) < 0.5f);
                // the boss's forward points at the camera: dot(forward, camera dir) > 0 = its face is seen
                assert(std::cos(bossYaw * kD2R) * p.x + std::sin(bossYaw * kD2R) * p.y > 0);
            }
        // walls: the player's side when clear enough, else the clearest, the preferred one winning ties
        const float open[kFaceCandidates] = {0.95f, 1, 1, 1, 1, 1};
        const float walled[kFaceCandidates] = {0.3f, 0.5f, 0.8f, 0.8f, 0.2f, 1};
        const float shut[kFaceCandidates] = {0.2f, 0.2f, 0.2f, 0.2f, 0.2f, 0.2f};
        assert(Best(open, kFaceCandidates) == 0 && Best(walled, kFaceCandidates) == 5 && Best(shut, kFaceCandidates) == 0);
        for (int i = 0; i < kFaceCandidates; i++) assert(std::fabs(Side(i, 33)) <= kFaceArc);
        // the orbit turns towards the front; even after a flip the camera stays within kFaceArc + Orbit(kTotal) of it
        assert(OrbitDir(40) < 0 && OrbitDir(-40) > 0);
        for (float s : {-kFaceArc, kFaceArc}) {
            float yaw = FaceYaw(0, s), dir = OrbitDir(s);
            Flip(yaw, dir, kApproach);
            assert(std::fabs(Wrap(yaw + dir * Orbit(kTotal) + 180)) <= kFaceArc + Orbit(kTotal) + 0.01f);
            assert(kFaceArc + Orbit(kTotal) < 90);
        }
    }
    {   // title: hidden outside the hold, full in the middle, soft in and out; skip / game splash fades it
        using namespace title;
        assert(At(0).alpha == 0 && At(kIn - 0.01f).alpha == 0 && At(kOut).alpha == 0);
        const Look mid = At((kIn + kOut) / 2);
        assert(mid.alpha > 0.99f && mid.rise < 0.01f && mid.divider > 0.99f);
        assert(At(kIn + 0.05f).alpha < 0.5f && At(kIn + 0.05f).rise > 0);  // fades in, rising
        assert(At(kOut - 0.05f).alpha < 0.5f);                              // fades out in place
        assert(Fade(-1) == 1 && Fade(0) == 1 && Fade(kSkip) == 0 && Fade(kSkip / 2) > 0.4f);
        assert(FromClass("BP_BossFight_CricTheThief_2nd_C") == "Cric The Thief 2nd");
        assert(FromClass("BP_BossFight_SkeletonLord_C") == "Skeleton Lord");
        assert(FromClass("BP_FinalBossFight_C") == "Final Boss Fight");
        assert(FromClass("") == "");
        assert(Epithet("Started combat with {FightName}!", "Lady Everleen") == "Started combat with Lady Everleen!");  // #73
        assert(Epithet("{Other} awakens", "X") == "" && Epithet("", "X") == "" && Epithet("The Unbroken", "X") == "The Unbroken");
    }
    std::puts("boss-intro: ok");
}

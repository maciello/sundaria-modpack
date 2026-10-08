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
        assert(d.On(Signal::SpawnTrigger, 1, 11).kind == Verdict::Same);
        assert(d.On(Signal::CombatStart, 1, 12).kind == Verdict::Same);
        assert(d.On(Signal::Splash, 0, 12.5).kind == Verdict::Same);  // the game's splash belongs to this intro
        assert(d.On(Signal::Lens, 0, 40).kind == Verdict::Same);       // later, but the fight already had its intro
        // wipe: the fight ends and restarts -> fires again
        assert(d.On(Signal::Finished, 1, 60).kind == Verdict::Rearm);
        assert(d.On(Signal::CombatStart, 1, 90).kind == Verdict::Start);
    }
    {   // two fights in one dungeon: each gets its own intro
        Detector d;
        assert(d.On(Signal::CombatStart, 1, 0).kind == Verdict::Start);
        assert(d.On(Signal::CombatStart, 2, 100).kind == Verdict::Start);
        assert(d.On(Signal::CombatStart, 1, 101).kind == Verdict::Same);
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
        // small bosses are not shot from inside their capsule
        const Pose tiny = Shot(0, 0, 0, 10, 0, 90);
        assert(std::sqrt(tiny.x * tiny.x + tiny.y * tiny.y) > kMinDist * 0.9f);
        assert(near(YawTo(0, 0, 0, 100), 90));
    }
    std::puts("boss-intro: ok");
}

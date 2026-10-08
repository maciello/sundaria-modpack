// just test
#include "boss-intro.hpp"
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
    std::puts("boss-intro: ok");
}

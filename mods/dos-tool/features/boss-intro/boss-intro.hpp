#pragma once
#include <algorithm>
#include <cstdint>
#include <vector>

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
}

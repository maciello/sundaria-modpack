#pragma once
#include <cstdint>
#include <string>
#include <vector>
#include "boss-intro.hpp"

// Game side of the boss intro (signals.cpp, the feature's only SDK unit). A ProcessEvent listener records the game's
// own boss signals on the game thread; the render thread takes them. No polling, no GObjects walk.
namespace boss_intro::game_side {
    struct Event {
        Signal signal;
        std::uintptr_t fight;     // fight actor id (0 = the signal carries none); never dereferenced outside signals.cpp
        std::string fightClass;   // BP_BossFight_<Boss>_C
        std::string name, subtitle;  // FightDisplayName, FightStartedMessage ("" if unset)
    };
    struct Boss { float x, y, z, halfHeight; };  // capsule centre (cm), capsule half height
    // The fight's first boss actor, if spawned (render thread: memory reads only).
    bool BossOf(std::uintptr_t fight, Boss& out);
    bool Alive(std::uintptr_t fight);  // the fight actor still exists (map travel frees it)
    std::uintptr_t LocalPawn();  // id of the local pawn (= combat::Sample::id), 0 = none (render thread: memory reads)
    void Listen(bool on);
    // #70: freeze the fight's bosses, partners and their controllers that this machine simulates (host / solo; a
    // co-op client simulates none, so it writes nothing). Idempotent per frame; returns how many were newly frozen.
    int Pause(std::uintptr_t fight);
    int Resume();  // restores all frozen actors still alive; returns how many
    std::vector<Event> Take();  // render thread: events since the last call
}

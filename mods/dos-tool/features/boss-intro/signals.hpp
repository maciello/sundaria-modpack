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
    std::uintptr_t LocalPawn();  // id of the local pawn (= combat::Sample::id), 0 = none (render thread: memory reads)
    void Listen(bool on);
    std::vector<Event> Take();  // render thread: events since the last call
}

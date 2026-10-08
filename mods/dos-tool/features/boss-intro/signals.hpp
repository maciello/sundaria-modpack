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
    // The fight's first boss actor, if spawned: read on the game thread in each camera update while asked (#80); this
    // returns the last read (one frame behind). StopSweep ends the asking.
    bool BossOf(std::uintptr_t fight, Boss& out);
    bool Alive(std::uintptr_t fight);  // the fight actor still exists (map travel frees it)
    std::uintptr_t LocalPawn();  // id of the local pawn (= combat::Sample::id), 0 = none (render thread: memory reads)
    void Listen(bool on);
    // #70: freeze the fight's bosses, partners and their controllers that this machine simulates (host / solo; a
    // co-op client simulates none, so it writes nothing). Asked every frame, done on the game thread in the camera
    // update; returns how many were newly frozen since the last call.
    int Pause(std::uintptr_t fight);
    // Restores all frozen actors still alive on the game thread (game::Drain: bounded wait, else here); returns how many.
    int Resume();
    // #71: render thread asks for a sphere sweep (cam::kProbe, Camera channel) from `from` to `to`, run on the game thread
    // in each camera update until StopSweep; the fight's bosses, partners and the local pawn are ignored.
    void Sweep(std::uintptr_t fight, const float from[3], const float to[3]);
    float Clear();  // share of from -> to that was clear in the last sweep (1 = nothing hit)
    void StopSweep();
    std::vector<Event> Take();  // render thread: events since the last call
}

#pragma once
#include <string>
#include <vector>
#include "combat.hpp"
#include "../features/ability-dump/ability-dump.hpp"

// Thin, SDK-free interface over the game. game.cpp is the ONLY translation unit
// that pulls in the (very large) generated Dumper-7 SDK headers.
namespace game {
    struct Snapshot {
        bool valid = false;
        std::string worldName;
        int objectCount = 0;

        // Player camera (BP_PlayerCamera_C / APlayerCameraManager)
        bool  haveCamera = false;
        float fov = 0.0f;        // DefaultFOV
        float distance = 0.0f;   // kInitialOrbitDistance (orbit distance)
    };

    // Blocks until UWorld + a local player exist (or timeout).
    bool WaitForEngine(int timeoutMs);

    // Read current live values for the menu readout / to seed sliders.
    Snapshot Gather();

    // Setters. Safe to call every frame (from the present hook) — they no-op if
    // the relevant object isn't present yet. Setting the "source" values the
    // camera blueprint reads each frame avoids the flicker we saw poking derived
    // values (TargetArmLength).
    void SetFOV(float degrees);            // APlayerCameraManager::DefaultFOV
    void SetCameraDistance(float units);   // BP_PlayerCamera_C::kInitialOrbitDistance
    void SetCameraCollision(bool enabled); // USpringArmComponent::bDoCollisionTest

    // Original (unmodified) values captured the first time we see the objects, so
    // the UI can seed its sliders and "reset" can restore them.
    // Damage numbers: live enemies' health + world position, and world -> screen.
    std::vector<combat::Sample> SampleHealth();
    bool GetView(combat::View& out);

    // CharacterMovement falling params, written to EVERY player character (movement is
    // server-simulated: the host's values decide in co-op). nullptr = restore each original.
    struct Movement { float airControl, boostMultiplier, boostThreshold, lateralFriction, brakingFalling; };
    void ApplyMovement(const Movement* m);
    bool OriginalMovement(Movement& out);  // local player's vanilla values, once seen

    // Debug: hook UObject::ProcessEvent and log each UFunction the first time it fires
    // (game thread records pointers; names are resolved by ProbeFlush on the render thread).
    void SetEventProbe(bool on);
    void ProbeFlush();

    // Game-thread ProcessEvent listener for a feature with its own SDK-including .cpp; runs after
    // the original call (obj = UObject*, fn = UFunction*). The hook stays installed while the probe
    // or any listener is on. Up to 4 listeners.
    using EventListener = void (*)(void* obj, void* fn, void* parms);
    void SetEventListener(EventListener l, bool on);

    float OriginalFOV();
    float OriginalDistance();

    // Ability dump (dev tool): every loaded ability class default object + the local player's playing montage.
    // Memory reads only (no ProcessEvent), but walks all of GObjects: call once per button press.
    void DumpAbilities(std::vector<ability_dump::Ability>& out, ability_dump::Live& live);
}

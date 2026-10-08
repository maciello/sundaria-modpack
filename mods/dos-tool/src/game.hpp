#pragma once
#include <string>
#include <vector>
#include "dmgnum.hpp"

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
    std::vector<dmgnum::Sample> SampleHealth();
    bool Project(float x, float y, float z, float& sx, float& sy);

    float OriginalFOV();
    float OriginalDistance();
}

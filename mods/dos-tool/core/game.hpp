#pragma once
#include <string>
#include <vector>
#include "combat.hpp"

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

    // CharacterMovement ground params, same rules as Movement (every player character, host wins).
    struct Ground { float maxAccel, brakingWalking, groundFriction, brakingFrictionFactor, jumpZ; };
    // f maps each character's own vanilla values to what to write; nullptr = restore each original.
    using GroundFn = Ground (*)(const Ground& vanilla, const void* ctx);
    void ApplyGround(GroundFn f, const void* ctx);
    bool OriginalGround(Ground& out);  // local player's vanilla values, once seen
    float LocalSpeed();                // local pawn's horizontal speed (cm/s), 0 outside gameplay

    // Debug: hook UObject::ProcessEvent and log each UFunction the first time it fires
    // (game thread records pointers; names are resolved by ProbeFlush on the render thread).
    void SetEventProbe(bool on);
    void ProbeFlush();

    // Game-thread ProcessEvent listener for a feature with its own SDK-including .cpp; runs after
    // the original call (obj = UObject*, fn = UFunction*). The hook stays installed while the probe
    // or any listener is on. Up to 4 listeners.
    using EventListener = void (*)(void* obj, void* fn, void* parms);
    void SetEventListener(EventListener l, bool on);

    // Gameplay effects (game thread only: these call UFunctions). asc = UAbilitySystemComponent*,
    // ability = UGameplayAbility*. Cooldown effects of one ability: class IsA its cooldown GE (often one
    // shared BP_GameplayEffect_Cooldown) AND the effect context names this ability, so other cooldowns stay.
    struct EffectRef { int handle; float duration; };
    int CooldownEffects(void* asc, void* ability, EffectRef* out, int max);
    bool RemoveEffect(void* asc, int handle);
    // Free camera: while set, every camera manager's BlueprintUpdateCamera result (hub, lobby and gameplay
    // cameras all override it) is replaced with this pose on the game thread. nullptr = off.
    struct CamPose { float x, y, z, pitch, yaw; };
    // priority 1 (free camera) outranks 0 (a follow camera): while both are set, the priority-1 pose shows.
    void SetFreeCam(const CamPose* pose, int priority = 1);
    int CamOwner();  // highest priority currently set, -1 = none
    int FreeCamOverrides();  // BlueprintUpdateCamera calls replaced so far (0 while on = that path never runs here)
    // Hub walk: possess the hub's hero (the player character standing in the village) and drive it.
    // Runs on the game thread; the render thread posts input every frame. nullptr = stop, give possession back.
    struct WalkInput { float moveX, moveY; bool jump; };  // world-space XY direction, length 0..1
    void SetHubWalk(const WalkInput* in);
    struct Hero { bool found = false, possessed = false; float x = 0, y = 0, z = 0, yaw = 0; int moveMode = -1, possessTries = 0, modeFixes = 0; };
    Hero HubHero();  // memory reads

    // Characters you can talk to (class name starts with "NPC_") + the click zone next to each, paired at first sight.
    struct Npc { uintptr_t id; std::string name; float x, y, z, yaw; bool hasButton; };
    std::vector<Npc> ListNpcs();  // memory reads; name = class without "NPC_"/"_C"
    // Move an NPC and its click zone (same offset as in vanilla); runs on the game thread.
    void PlaceNpc(uintptr_t id, float x, float y, float z, float yaw);

    // The hub was built to be looked at: many meshes have no collision. Within `radius` of (x,y,z), on the game thread:
    // log each static mesh's collision setup once, and with fix=true switch collision on (blocking everything;
    // meshes without simple collision use their triangles). Each component is handled once per map.
    void FixCollision(float x, float y, float z, float radius, bool fix);

    // Closed room: the building (collision-less hub mesh) around world point (x,y) gets an invisible floor at
    // z (feet height if zIsFeet, else a character centre: minus the hub hero's capsule half height) and four walls
    // `inset` inside its bounds. Replaces that building's previous room; teleportHero puts the hub hero onto the
    // floor at (x,y). Game thread; result text via RoomStatus().
    // insets: world units each wall moves in from the building bounds, in the building's own axes (-X, +X, -Y, +Y)
    struct RoomInsets { float minX, maxX, minY, maxY; };
    void MakeRoom(float x, float y, float z, bool zIsFeet, const RoomInsets& insets, float wallHeight, bool teleportHero);
    std::string RoomStatus();
    void ShowRoom(bool show);  // draw the room's invisible boxes as outlines

    // Survey: log "class name @ x y z" of every actor in the loaded levels (memory reads only). Returns the count.
    int LogActors();
    // Debug readout: camera manager class + its view target class ("-" when missing).
    void CameraClasses(std::string& manager, std::string& target);

    float OriginalFOV();
    float OriginalDistance();

    // One-time singleton lookup (the only allowed GObjects walk outside dev probes): first live, non-default
    // object whose class is named className; logs its cost in ms. Callers cache the result. Game thread.
    void* FindSingleton(const char* className);
}

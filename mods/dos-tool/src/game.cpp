#include "game.hpp"
#include "logger.hpp"

#include <Windows.h>
#include <cstdint>

// game.cpp is the ONLY translation unit that pulls in the generated SDK.
#include "Engine_classes.hpp"
#include "Archon_classes.hpp"          // UArchonSpringArmComponent
#include "BP_PlayerCamera_classes.hpp" // ABP_PlayerCamera_C (kInitialOrbitDistance)
#include "GameplayAbilities_classes.hpp" // UAbilitySystemComponent::SpawnedAttributes

using namespace SDK;

namespace {
    inline bool PtrOk(const void* p) {
        const uintptr_t v = reinterpret_cast<uintptr_t>(p);
        return v > 0x10000 && v < 0x7FFFFFFFFFFFull;
    }

    // Originals captured the first time we see each object, so the UI can seed
    // its sliders and restore on reset.
    bool  g_haveOrigFov = false;   float g_origFov = 90.0f;
    bool  g_haveOrigDist = false;  float g_origDist = 650.0f;

    APlayerController* LocalPC() {
        UWorld* w = UWorld::GetWorld();
        if (!PtrOk(w)) return nullptr;
        UGameInstance* gi = w->OwningGameInstance;
        if (!PtrOk(gi)) return nullptr;
        auto& lps = gi->LocalPlayers;
        if (lps.Num() <= 0 || !PtrOk(lps[0])) return nullptr;
        return lps[0]->PlayerController;
    }

    // The gameplay player camera IS a BP_PlayerCamera_C. In menus/lobby the camera
    // manager is a different class, so verify with IsA before casting — otherwise
    // reading kInitialOrbitDistance/ArchonSpringArm at those offsets faults.
    ABP_PlayerCamera_C* PlayerCam() {
        APlayerController* pc = LocalPC();
        if (!PtrOk(pc)) return nullptr;
        APlayerCameraManager* cam = pc->PlayerCameraManager;
        if (!PtrOk(cam)) return nullptr;
        if (!cam->IsA(ABP_PlayerCamera_C::StaticClass())) return nullptr;
        return static_cast<ABP_PlayerCamera_C*>(cam);
    }
}

bool game::WaitForEngine(int timeoutMs) {
    int waited = 0;
    while (waited < timeoutMs) {
        UWorld* w = UWorld::GetWorld();
        if (PtrOk(w) && PtrOk(w->OwningGameInstance)
            && UObject::GObjects && UObject::GObjects->Num() > 0)
            return true;
        Sleep(200);
        waited += 200;
    }
    return false;
}

game::Snapshot game::Gather() {
    Snapshot s;
    UWorld* world = UWorld::GetWorld();
    if (!PtrOk(world)) return s;
    s.valid = true;
    if (UObject::GObjects) s.objectCount = UObject::GObjects->Num();
    s.worldName = world->GetName();

    if (ABP_PlayerCamera_C* cam = PlayerCam()) {
        s.haveCamera = true;
        s.fov = cam->DefaultFOV;
        s.distance = cam->kInitialOrbitDistance;
        if (!g_haveOrigFov)  { g_origFov = cam->DefaultFOV; g_haveOrigFov = true; }
        if (!g_haveOrigDist) { g_origDist = cam->kInitialOrbitDistance; g_haveOrigDist = true; }
    }
    return s;
}

void game::SetFOV(float degrees) {
    if (ABP_PlayerCamera_C* cam = PlayerCam())
        cam->DefaultFOV = degrees;
}

void game::SetCameraDistance(float units) {
    if (ABP_PlayerCamera_C* cam = PlayerCam())
        cam->kInitialOrbitDistance = units;
}

void game::SetCameraCollision(bool enabled) {
    if (ABP_PlayerCamera_C* cam = PlayerCam()) {
        if (PtrOk(cam->ArchonSpringArm))
            cam->ArchonSpringArm->bDoCollisionTest = enabled ? 1 : 0;
    }
}

// Memory reads only (no ProcessEvent): this runs on the render thread.
// ponytail: walks every actor of every loaded level each frame; cache the character list if it shows in frame time
std::vector<dmgnum::Sample> game::SampleHealth() {
    std::vector<dmgnum::Sample> out;
    UWorld* w = UWorld::GetWorld();
    if (!PtrOk(w)) return out;
    UClass* charCls = AArchonCharacter::StaticClass();
    UClass* statusCls = UArchonAttributeSet_Status::StaticClass();
    for (int li = 0; li < w->Levels.Num(); li++) {
        ULevel* lvl = w->Levels[li];
        if (!PtrOk(lvl)) continue;
        for (int ai = 0; ai < lvl->Actors.Num(); ai++) {
            AActor* a = lvl->Actors[ai];
            if (!PtrOk(a) || !a->IsA(charCls)) continue;
            auto* c = static_cast<AArchonCharacter*>(a);
            UArchonAbilitySystemComponent* asc = c->mAbilitySystemComponent;
            USceneComponent* root = c->RootComponent;
            if (!PtrOk(asc) || !PtrOk(root)) continue;
            auto& sets = asc->SpawnedAttributes;
            for (int si = 0; si < sets.Num(); si++) {
                UAttributeSet* set = sets[si];
                if (!PtrOk(set) || !set->IsA(statusCls)) continue;
                const FVector& p = root->RelativeLocation;  // capsule center; unattached root: relative == world
                out.push_back({reinterpret_cast<uintptr_t>(c), p.X, p.Y, p.Z,
                               static_cast<UArchonAttributeSet_Status*>(set)->CurrentHealth,
                               PtrOk(c->PlayerState)});
                break;
            }
        }
    }
    return out;
}

bool game::GetView(dmgnum::View& v) {
    APlayerController* pc = LocalPC();
    if (!PtrOk(pc) || !PtrOk(pc->PlayerCameraManager)) return false;
    const FMinimalViewInfo& pov = pc->PlayerCameraManager->CameraCachePrivate.POV;
    v = {pov.Location.X, pov.Location.Y, pov.Location.Z,
         pov.Rotation.Pitch, pov.Rotation.Yaw, pov.Rotation.Roll, pov.FOV};
    return v.fov > 1.0f;
}

float game::OriginalFOV()      { return g_origFov; }
float game::OriginalDistance() { return g_origDist; }

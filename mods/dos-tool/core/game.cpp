#include "game.hpp"
#include "logger.hpp"

#include <Windows.h>
#include <cstdint>
#include <unordered_map>
#include <unordered_set>
#include <atomic>
#include <vector>
#include <cstdio>
#include "minhook/include/MinHook.h"

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
std::vector<combat::Sample> game::SampleHealth() {
    std::vector<combat::Sample> out;
    UWorld* w = UWorld::GetWorld();
    if (!PtrOk(w)) return out;
    UClass* charCls = AArchonCharacter::StaticClass();
    UClass* statusCls = UArchonAttributeSet_Status::StaticClass();
    UClass* secondaryCls = UArchonAttributeSet_Secondary::StaticClass();
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
            const UArchonAttributeSet_Status* status = nullptr;
            const UArchonAttributeSet_Secondary* secondary = nullptr;
            auto& sets = asc->SpawnedAttributes;
            for (int si = 0; si < sets.Num(); si++) {
                UAttributeSet* set = sets[si];
                if (!PtrOk(set)) continue;
                if (set->IsA(statusCls)) status = static_cast<UArchonAttributeSet_Status*>(set);
                else if (set->IsA(secondaryCls)) secondary = static_cast<UArchonAttributeSet_Secondary*>(set);
            }
            if (!status) continue;
            // UPrimitiveComponent::LastRenderTimeOnScreen: native field in Dumper-7's Pad_1F8 (UE 4.27 layout, unverified)
            static_assert(offsetof(UPrimitiveComponent, MinDrawDistance) == 0x200, "re-check LastRenderTimeOnScreen offset");
            const float seen = PtrOk(c->Mesh) ? *reinterpret_cast<const float*>(reinterpret_cast<const uint8*>(c->Mesh) + 0x1FC) : 0.0f;
            const FVector& p = root->RelativeLocation;  // capsule center; unattached root: relative == world
            out.push_back({reinterpret_cast<uintptr_t>(c), p.X, p.Y, p.Z, status->CurrentHealth,
                           PtrOk(c->PlayerState), secondary ? secondary->Health : 0.0f, status->CurrentLevel, seen});
        }
    }
    return out;
}

bool game::GetView(combat::View& v) {
    APlayerController* pc = LocalPC();
    if (!PtrOk(pc) || !PtrOk(pc->PlayerCameraManager)) return false;
    const FMinimalViewInfo& pov = pc->PlayerCameraManager->CameraCachePrivate.POV;
    v = {pov.Location.X, pov.Location.Y, pov.Location.Z,
         pov.Rotation.Pitch, pov.Rotation.Yaw, pov.Rotation.Roll, pov.FOV};
    return v.fov > 1.0f;
}

namespace {
    // ponytail: keyed by pointer, never pruned while on (a few entries per level); a reused address keeps the old original
    std::unordered_map<UCharacterMovementComponent*, game::Movement> g_origMove;
    bool g_haveLocalMove = false; game::Movement g_localMove{};

    template <class F> void ForEachPlayerMovement(F&& fn) {
        UWorld* w = UWorld::GetWorld();
        if (!PtrOk(w)) return;
        UClass* charCls = ACharacter::StaticClass();
        for (int li = 0; li < w->Levels.Num(); li++) {
            ULevel* lvl = w->Levels[li];
            if (!PtrOk(lvl)) continue;
            for (int ai = 0; ai < lvl->Actors.Num(); ai++) {
                AActor* a = lvl->Actors[ai];
                if (!PtrOk(a) || !a->IsA(charCls)) continue;
                auto* c = static_cast<ACharacter*>(a);
                if (!PtrOk(c->PlayerState) || !PtrOk(c->CharacterMovement)) continue;
                fn(c, c->CharacterMovement);
            }
        }
    }
}

void game::ApplyMovement(const Movement* m) {
    APlayerController* pc = LocalPC();
    APawn* local = PtrOk(pc) ? pc->Pawn : nullptr;
    // Restore only components found in the live world: stale map keys may be freed.
    ForEachPlayerMovement([&](ACharacter* c, UCharacterMovementComponent* cm) {
        auto it = g_origMove.find(cm);
        if (!m) {
            if (it == g_origMove.end()) return;
            const Movement& o = it->second;
            cm->AirControl = o.airControl; cm->AirControlBoostMultiplier = o.boostMultiplier;
            cm->AirControlBoostVelocityThreshold = o.boostThreshold;
            cm->FallingLateralFriction = o.lateralFriction; cm->BrakingDecelerationFalling = o.brakingFalling;
            return;
        }
        if (it == g_origMove.end()) {
            const Movement o{cm->AirControl, cm->AirControlBoostMultiplier, cm->AirControlBoostVelocityThreshold,
                             cm->FallingLateralFriction, cm->BrakingDecelerationFalling};
            g_origMove.emplace(cm, o);
            if (c == local) { g_localMove = o; g_haveLocalMove = true; }
        }
        cm->AirControl = m->airControl; cm->AirControlBoostMultiplier = m->boostMultiplier;
        cm->AirControlBoostVelocityThreshold = m->boostThreshold;
        cm->FallingLateralFriction = m->lateralFriction; cm->BrakingDecelerationFalling = m->brakingFalling;
    });
    if (!m) g_origMove.clear();
}

bool game::OriginalMovement(Movement& out) { out = g_localMove; return g_haveLocalMove; }

float game::OriginalFOV()      { return g_origFov; }
float game::OriginalDistance() { return g_origDist; }

namespace {
    using ProcessEvent_t = void (*)(const UObject*, UFunction*, void*);
    ProcessEvent_t g_oPE = nullptr;
    void* g_peTarget = nullptr;
    std::atomic<bool> g_probeOn{false};
    std::atomic<int> g_inPE{0};
    SRWLOCK g_probeMu = SRWLOCK_INIT;  // not std::mutex: newer STL's constexpr mutex null-derefs in Proton's older msvcp140 (crashed the game)
    std::unordered_set<UFunction*> g_seen;
    struct Fired { UFunction* fn; UClass* cls; ULONGLONG t; };
    std::vector<Fired> g_fresh;

    void hkProcessEvent(const UObject* obj, UFunction* fn, void* parms) {
        g_inPE++;
        if (g_probeOn.load(std::memory_order_relaxed)) {
            AcquireSRWLockExclusive(&g_probeMu);
            if (g_seen.insert(fn).second)
                g_fresh.push_back({fn, PtrOk(obj) ? obj->Class : nullptr, GetTickCount64()});
            ReleaseSRWLockExclusive(&g_probeMu);
        }
        g_oPE(obj, fn, parms);
        g_inPE--;
    }
}

void game::SetEventProbe(bool on) {
    if (on && !g_peTarget) {
        MH_Initialize();  // already initialised by kiero: harmless
        void* target = reinterpret_cast<void*>(InSDKUtils::GetImageBase() + Offsets::ProcessEvent);
        if (MH_CreateHook(target, (void*)hkProcessEvent, (void**)&g_oPE) != MH_OK || MH_EnableHook(target) != MH_OK) {
            logger::log("[probe] ProcessEvent hook failed");
            return;
        }
        g_peTarget = target;
        logger::log("[probe] ProcessEvent hooked");
    }
    g_probeOn = on;
    if (!on && g_peTarget) {
        MH_DisableHook(g_peTarget);
        MH_RemoveHook(g_peTarget);
        g_peTarget = nullptr;
        for (int i = 0; i < 200 && g_inPE.load() > 0; i++) Sleep(10);  // let in-flight calls leave our code before unload
    }
}

void game::ProbeFlush() {
    std::vector<Fired> batch;
    {
        AcquireSRWLockExclusive(&g_probeMu);
        batch.swap(g_fresh);
        ReleaseSRWLockExclusive(&g_probeMu);
    }
    for (const Fired& f : batch) {
        char buf[512];
        std::snprintf(buf, sizeof(buf), "[probe] t=%llu fn=%s on=%s", f.t,
                      PtrOk(f.fn) ? f.fn->GetFullName().c_str() : "?", PtrOk(f.cls) ? f.cls->GetName().c_str() : "?");
        logger::log(buf);
    }
}

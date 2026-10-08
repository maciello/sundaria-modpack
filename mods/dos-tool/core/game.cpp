#include "game.hpp"
#include "logger.hpp"
#include "ref.hpp"

#include <Windows.h>
#include <cstdint>
#include <unordered_map>
#include <unordered_set>
#include <atomic>
#include <vector>
#include <cstdio>
#include <cmath>
#include "minhook/include/MinHook.h"

// game.cpp is the ONLY translation unit that pulls in the generated SDK.
#include "Engine_classes.hpp"
#include "Archon_classes.hpp"          // UArchonSpringArmComponent
#include "BP_PlayerCamera_classes.hpp" // ABP_PlayerCamera_C (kInitialOrbitDistance)
#include "GameplayAbilities_classes.hpp" // UAbilitySystemComponent::SpawnedAttributes
#include "GameplayAbilities_parameters.hpp"
#include "Archon_parameters.hpp"
#include "Engine_parameters.hpp"
#include "room.hpp"
#include <cmath>
#include <algorithm>        // Params::PlayerCameraManager_BlueprintUpdateCamera (free camera)

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

namespace {
    // Damage-type class → element, by class name; resolved once per class (render thread only).
    combat::Element ElementOf(UClass* type) {
        static std::unordered_map<ref::Ref, combat::Element, ref::Hash> cache;  // ponytail: entries of collected classes stay (a few)
        if (!type) return combat::Element::Physical;
        const ref::Ref key(type);
        auto it = cache.find(key);
        if (it == cache.end()) it = cache.emplace(key, combat::Classify(type->GetName())).first;
        return it->second;
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
            // UPrimitiveComponent::LastRenderTimeOnScreen: native, inside Dumper-7's Pad_288 after BoundsScale
            // (UE 4.27: LastSubmitTime 0x288, LastRenderTime 0x28C, LastRenderTimeOnScreen 0x290; all three advance in-game)
            static_assert(offsetof(UPrimitiveComponent, BoundsScale) == 0x284, "re-check LastRenderTimeOnScreen offset");
            const float seen = PtrOk(c->Mesh) ? *reinterpret_cast<const float*>(reinterpret_cast<const uint8*>(c->Mesh) + 0x290) : 0.0f;
            const FVector& p = root->RelativeLocation;  // capsule center; unattached root: relative == world
            // Last hit as the game records it (replicated, so clients see it too): plain memory, no hook.
            const FTakeHitInfo& hit = c->LastTakeHitInfo;
            UClass* type = PtrOk(hit.DamageTypeClass) ? hit.DamageTypeClass : nullptr;
            const uintptr_t by = reinterpret_cast<uintptr_t>(hit.PawnInstigator.Get());  // weak ptr: GObjects lookup, memory only
            const uintptr_t source = type ? reinterpret_cast<uintptr_t>(type) * 31 + by : 0;  // damage type × who: one stack each
            out.push_back({reinterpret_cast<uintptr_t>(c), p.X, p.Y, p.Z, status->CurrentHealth,
                           PtrOk(c->PlayerState), secondary ? secondary->Health : 0.0f, status->CurrentLevel, seen,
                           hit.EnsureReplicationByte, source, ElementOf(type), hit.ActualDamage});
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
    // ponytail: never pruned while on (a few entries per level)
    std::unordered_map<ref::Ref, game::Movement, ref::Hash> g_origMove;
    bool g_haveLocalMove = false; game::Movement g_localMove{};
    std::unordered_map<ref::Ref, game::Ground, ref::Hash> g_origGround;
    bool g_haveLocalGround = false; game::Ground g_localGround{};

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
        auto it = g_origMove.find(ref::Ref(cm));
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
            g_origMove.emplace(ref::Ref(cm), o);
            if (c == local) { g_localMove = o; g_haveLocalMove = true; }
        }
        cm->AirControl = m->airControl; cm->AirControlBoostMultiplier = m->boostMultiplier;
        cm->AirControlBoostVelocityThreshold = m->boostThreshold;
        cm->FallingLateralFriction = m->lateralFriction; cm->BrakingDecelerationFalling = m->brakingFalling;
    });
    if (!m) g_origMove.clear();
}

bool game::OriginalMovement(Movement& out) { out = g_localMove; return g_haveLocalMove; }

void game::ApplyGround(GroundFn f, const void* ctx) {
    APlayerController* pc = LocalPC();
    APawn* local = PtrOk(pc) ? pc->Pawn : nullptr;
    auto write = [](UCharacterMovementComponent* cm, const Ground& v) {
        cm->MaxAcceleration = v.maxAccel; cm->BrakingDecelerationWalking = v.brakingWalking;
        cm->GroundFriction = v.groundFriction; cm->BrakingFrictionFactor = v.brakingFrictionFactor;
        cm->JumpZVelocity = v.jumpZ;
    };
    // Restore only components found in the live world: stale map keys may be freed.
    ForEachPlayerMovement([&](ACharacter* c, UCharacterMovementComponent* cm) {
        auto it = g_origGround.find(ref::Ref(cm));
        if (!f) {
            if (it != g_origGround.end()) write(cm, it->second);
            return;
        }
        if (it == g_origGround.end()) {
            const Ground o{cm->MaxAcceleration, cm->BrakingDecelerationWalking, cm->GroundFriction,
                           cm->BrakingFrictionFactor, cm->JumpZVelocity};
            it = g_origGround.emplace(ref::Ref(cm), o).first;
            if (c == local) { g_localGround = o; g_haveLocalGround = true; }
        }
        write(cm, f(it->second, ctx));
    });
    if (!f) g_origGround.clear();
}

bool game::OriginalGround(Ground& out) { out = g_localGround; return g_haveLocalGround; }

float game::LocalSpeed() {
    APlayerController* pc = LocalPC();
    if (!PtrOk(pc) || !PtrOk(pc->Pawn) || !pc->Pawn->IsA(ACharacter::StaticClass())) return 0.0f;
    auto* cm = static_cast<ACharacter*>(pc->Pawn)->CharacterMovement;
    if (!PtrOk(cm)) return 0.0f;
    return std::sqrt(cm->Velocity.X * cm->Velocity.X + cm->Velocity.Y * cm->Velocity.Y);
}

float game::OriginalFOV()      { return g_origFov; }
float game::OriginalDistance() { return g_origDist; }

namespace {
    using ProcessEvent_t = void (*)(const UObject*, UFunction*, void*);
    ProcessEvent_t g_oPE = nullptr;
    void* g_peTarget = nullptr;
    std::atomic<bool> g_probeOn{false};
    std::atomic<int> g_inPE{0};
    SRWLOCK g_probeMu = SRWLOCK_INIT;  // not std::mutex: newer STL's constexpr mutex null-derefs in Proton's older msvcp140 (crashed the game)
    std::unordered_set<ref::Ref, ref::Hash> g_seen;
    struct Fired { ref::Ref fn, cls; ULONGLONG t; };
    std::vector<Fired> g_fresh;
    std::atomic<game::EventListener> g_listeners[16] = {};  // 10 users today; full = logged, never silent
    std::atomic<game::EventFilter> g_filter{nullptr};  // ponytail: one slot (item-sort); a table when a second user comes

    // Free camera: render thread writes the pose, the game thread's BlueprintUpdateCamera call reads it.
    std::atomic<bool> g_freeOn{false};
    std::atomic<int> g_freeHits{0};
    std::atomic<int32> g_camFnName{-1};  // FName index of "BlueprintUpdateCamera" (BP overrides share it)
    SRWLOCK g_freeMu = SRWLOCK_INIT;
    game::CamPose g_freePose{};
    SRWLOCK g_gameCamMu = SRWLOCK_INIT;
    game::CamPose g_gameCam{};  // fovDelta = the game's absolute FOV here
    ULONGLONG g_gameCamAt = 0;
    std::atomic<uint32_t> g_poseSeq{0};
    std::atomic<DWORD> g_gameTid{0};      // UE's game thread owns the window (it pumps the messages)
    std::atomic<bool> g_camMoved{false};  // a view camera actor sits at the free pose and needs restoring

    // Hub/lobby views look through placed camera actors: the engine reads their transform directly and never
    // calls BlueprintUpdateCamera, so the free camera moves the view-target actor itself (game thread only).
    uint32_t g_appliedSeq = 0;
    bool g_inCamMove = false;
    ref::Ref g_movedCam;  // ACameraActor
    FVector g_camOrigLoc{};
    FRotator g_camOrigRot{};

    void RestoreViewCam() {
        if (auto* cam = g_movedCam.Get<ACameraActor>()) cam->K2_SetActorLocationAndRotation(g_camOrigLoc, g_camOrigRot, false, nullptr, true);
        g_movedCam = {};
        g_camMoved = false;
    }

    void MoveViewCam() {
        APlayerController* pc = LocalPC();
        if (!PtrOk(pc) || !PtrOk(pc->PlayerCameraManager)) return;
        AActor* t = pc->PlayerCameraManager->ViewTarget.Target;
        if (!PtrOk(t) || !t->IsA(ACameraActor::StaticClass())) return;
        if (!g_movedCam.Is(t)) {
            RestoreViewCam();  // the game switched views mid-flight: put the previous camera back
            g_movedCam = ref::Ref(t);
            g_camOrigLoc = t->K2_GetActorLocation();
            g_camOrigRot = t->K2_GetActorRotation();
            g_camMoved = true;
        }
        AcquireSRWLockShared(&g_freeMu);
        const game::CamPose p = g_freePose;
        ReleaseSRWLockShared(&g_freeMu);
        t->K2_SetActorLocationAndRotation(FVector{p.x, p.y, p.z}, FRotator{p.pitch, p.yaw, 0.0f}, false, nullptr, true);
        g_freeHits++;
    }

    // The game thread owns the game's window (UE: class "UnrealWindow"). Found by enumerating this process's
    // windows, so a hot reload while the game is in the background works at once (#78).
    bool EnsureGameTid() {
        if (g_gameTid.load()) return true;
        DWORD tid = 0;
        EnumWindows([](HWND w, LPARAM out) -> BOOL {
            DWORD pid = 0;
            const DWORD t = GetWindowThreadProcessId(w, &pid);
            char cls[32];
            if (pid != GetCurrentProcessId() || !GetClassNameA(w, cls, sizeof cls) || strcmp(cls, "UnrealWindow")) return TRUE;
            *reinterpret_cast<DWORD*>(out) = t;
            return FALSE;
        }, reinterpret_cast<LPARAM>(&tid));
        if (!tid) return false;  // window not created yet: try next frame
        g_gameTid = tid;
        return true;
    }

    // Hub walk: the render thread finds the hero (memory reads) and posts input; the game thread possesses + drives it.
    std::atomic<bool> g_walkOn{false};
    std::atomic<bool> g_walkHeld{false};  // we possessed the hero and owe the controller its previous pawn
    SRWLOCK g_walkMu = SRWLOCK_INIT;  // also guards g_hero
    ref::Ref g_hero;  // hub hero (AActor): found by the render thread, driven by the game thread
    game::WalkInput g_walkIn{};
    std::atomic<uint32_t> g_walkSeq{0};
    uint32_t g_walkApplied = 0;
    ref::Ref g_prevPawn;  // APawn; ptr null = the controller had none
    bool g_jumpWas = false;
    ULONGLONG g_lastPossess = 0;
    std::atomic<int> g_possessTries{0};
    std::atomic<int> g_modeFixes{0};  // times the movement mode was found None while walking and switched back on
    ULONGLONG g_lastModeFix = 0;
    // The hub parks its hero (no movement mode / no tick): switched on while walking, put back after.
    ref::Ref g_heldHero;  // ACharacter
    bool g_heroOrigOrient = false, g_heroOrigCtrlYaw = false;
    FRotator g_heroOrigRate{};

    AActor* HeroActor() {
        AcquireSRWLockShared(&g_walkMu);
        const ref::Ref r = g_hero;
        ReleaseSRWLockShared(&g_walkMu);
        return r.Get<AActor>();
    }

    void WalkTick() {
        AActor* h = HeroActor();
        APlayerController* pc = LocalPC();
        if (!h || !h->IsA(ACharacter::StaticClass()) || !PtrOk(pc)) return;
        auto* hero = static_cast<ACharacter*>(h);
        if (!g_heldHero.ptr) {
            g_heldHero = ref::Ref(hero);
            g_walkHeld = true;
            g_prevPawn = ref::Ref(pc->Pawn);
            if (UCharacterMovementComponent* cm = hero->CharacterMovement; PtrOk(cm)) {
                cm->bRunPhysicsWithNoController = 1;  // still moves if the hub refuses the possession
                if (cm->MovementMode == EMovementMode::MOVE_None) cm->SetMovementMode(EMovementMode::MOVE_Walking, 0);
                cm->SetComponentTickEnabled(true);
                // turn towards where it walks (the hub's hero is set up to face a fixed way)
                g_heroOrigOrient = cm->bOrientRotationToMovement;
                g_heroOrigRate = cm->RotationRate;
                cm->bOrientRotationToMovement = 1;
                if (cm->RotationRate.Yaw < 1.0f) cm->RotationRate.Yaw = 540.0f;
            }
            g_heroOrigCtrlYaw = hero->bUseControllerRotationYaw;
            hero->bUseControllerRotationYaw = 0;
            hero->SetActorTickEnabled(true);
        }
        // something (the hub's own logic) can park the hero again mid-walk: keep it walkable
        if (UCharacterMovementComponent* cm = hero->CharacterMovement; PtrOk(cm) && cm->MovementMode == EMovementMode::MOVE_None) {
            const ULONGLONG now = GetTickCount64();
            if (now - g_lastModeFix >= 250) {
                g_lastModeFix = now;
                cm->SetComponentTickEnabled(true);
                cm->SetMovementMode(EMovementMode::MOVE_Walking, 0);
                g_modeFixes++;
            }
        }
        if (pc->Pawn != hero) {
            const ULONGLONG now = GetTickCount64();
            if (now - g_lastPossess >= 1000 && g_possessTries.load() < 5) {  // retry once a second, give up after 5
                g_lastPossess = now;
                g_possessTries++;
                pc->Possess(hero);
                pc->ResetIgnoreMoveInput();
            }
        }
        AcquireSRWLockShared(&g_walkMu);
        const game::WalkInput in = g_walkIn;
        ReleaseSRWLockShared(&g_walkMu);
        if (in.moveX != 0.0f || in.moveY != 0.0f) hero->AddMovementInput(FVector{in.moveX, in.moveY, 0.0f}, 1.0f, true);
        if (in.jump && !g_jumpWas) hero->Jump();
        if (!in.jump && g_jumpWas) hero->StopJumping();
        g_jumpWas = in.jump;
    }

    void WalkRelease() {
        APlayerController* pc = LocalPC();
        auto* held = g_heldHero.Get<ACharacter>();
        if (PtrOk(pc) && held && pc->Pawn == held) {
            if (auto* prev = g_prevPawn.Get<APawn>(); prev && prev != held) pc->Possess(prev);
            else if (!g_prevPawn.ptr) pc->UnPossess();
        }
        if (held) {  // settings back; the hero stays where it walked (e.g. in the tavern)
            if (UCharacterMovementComponent* cm = held->CharacterMovement; PtrOk(cm)) {
                cm->bRunPhysicsWithNoController = 0;
                cm->bOrientRotationToMovement = g_heroOrigOrient ? 1 : 0;
                cm->RotationRate = g_heroOrigRate;
            }
            held->bUseControllerRotationYaw = g_heroOrigCtrlYaw ? 1 : 0;
        }
        g_heldHero = {};
        g_possessTries = 0;
        g_prevPawn = {};
        g_jumpWas = false;
        g_walkHeld = false;
    }

    // NPC placement: queued by the render thread, applied on the game thread.
    struct Placement { ref::Ref npc, button; FVector offset; FVector loc; float yaw; };  // AActor
    SRWLOCK g_placeMu = SRWLOCK_INIT;
    std::vector<Placement> g_placeQueue;
    std::atomic<bool> g_placePending{false};

    // Collision: requests queued by the render thread, handled on the game thread (UFunction calls).
    struct CollisionReq { FVector at; float radius; bool fix; };
    SRWLOCK g_colMu = SRWLOCK_INIT;
    std::vector<CollisionReq> g_colQueue;
    std::atomic<bool> g_colPending{false};
    std::unordered_set<ref::Ref, ref::Hash> g_colDone;  // UStaticMeshComponent. ponytail: cleared on world change only
    std::unordered_set<ref::Ref, ref::Hash> g_colLogged;  // UStaticMesh
    ref::Ref g_colWorld;

    // "<path>/x_HUB" → the same mesh without "_HUB" (the hub uses collision-less copies of world meshes), if it exists.
    std::unordered_map<ref::Ref, ref::Ref, ref::Hash> g_realMesh;  // ptr null = no real version

    int SimpleShapes(UBodySetup* bs) {
        if (!PtrOk(bs)) return -1;
        const FKAggregateGeom& g = bs->AggGeom;
        return g.SphereElems.Num() + g.BoxElems.Num() + g.SphylElems.Num() + g.ConvexElems.Num() + g.TaperedCapsuleElems.Num();
    }

    UStaticMesh* RealMesh(UStaticMesh* hub) {
        const ref::Ref key(hub);
        auto it = g_realMesh.find(key);
        if (it != g_realMesh.end() && (!it->second.ptr || it->second.Get())) return it->second.Get<UStaticMesh>();
        g_realMesh[key] = {};
        // the package's raw FName is the full path (/Game/…/x_HUB); GetName()/GetFullName() drop the folders
        std::string pkg = "";
        for (UObject* p = hub->Outer; PtrOk(p); p = p->Outer) pkg = p->Name.GetRawString();
        std::string name = hub->GetName();
        if (name.find("_HUB") == std::string::npos) return nullptr;
        for (size_t p; (p = name.find("_HUB")) != std::string::npos;) name.erase(p, 4);
        std::string folder = pkg.substr(0, pkg.find_last_of('/') + 1);
        for (size_t p; (p = folder.find("_HUB")) != std::string::npos;) folder.erase(p, 4);
        // twins seen in dungeons live in HumanTown/Meshes (mesh probe); the hub copies may sit elsewhere
        UStaticMesh* real = nullptr;
        for (const std::string& dir : {folder, std::string("/Game/Environments/HumanTown/Meshes/")}) {
            const std::string path = dir + name + "." + name;
            const std::wstring wpath(path.begin(), path.end());
            UObject* o = UKismetSystemLibrary::LoadAsset_Blocking(
                UKismetSystemLibrary::Conv_SoftObjPathToSoftObjRef(UKismetSystemLibrary::MakeSoftObjectPath(FString(wpath.c_str()))));
            real = PtrOk(o) && o->IsA(UStaticMesh::StaticClass()) ? static_cast<UStaticMesh*>(o) : nullptr;
            char buf[400];
            std::snprintf(buf, sizeof(buf), "[collision] real version %s (hub copy in %s): %s simple=%d trace=%d", path.c_str(), pkg.c_str(),
                          real ? "found" : "missing", real ? SimpleShapes(real->BodySetup) : -1,
                          real && PtrOk(real->BodySetup) ? int(real->BodySetup->CollisionTraceFlag) : -1);
            logger::log(buf);
            if (real) break;
        }
        g_realMesh[key] = ref::Ref(real);
        return real;
    }

    // Invisible copy of `smc` with the real (colliding) mesh, same world transform.
    // A component we add must be owned by something the garbage collector sees: added to an existing actor it is
    // referenced by nothing, gets collected, and a character that touched it crashes the game ("Object without
    // class referenced by BP_CharacterBase … OtherComp"). So each one is the root of its own spawned actor, which
    // the level keeps alive until the map unloads. `setup` runs before the component registers (mesh, mobility).
    template <class C, class F> C* SpawnRooted(AActor* worldCtx, const FTransform& world, F&& setup, AActor** holderOut) {
        AActor* holder = UGameplayStatics::BeginDeferredActorSpawnFromClass(worldCtx, AActor::StaticClass(), world,
                                                                            ESpawnActorCollisionHandlingMethod::AlwaysSpawn, nullptr);
        if (!PtrOk(holder)) return nullptr;
        UGameplayStatics::FinishSpawningActor(holder, world);
        auto* c = static_cast<C*>(holder->AddComponentByClass(C::StaticClass(), false, world, true));
        if (!PtrOk(c)) { holder->K2_DestroyActor(); return nullptr; }
        c->SetMobility(EComponentMobility::Movable);
        setup(c);
        holder->FinishAddComponent(c, false, world);  // no root yet: it becomes the root, relative == world
        if (holderOut) *holderOut = holder;
        return c;
    }

    bool AddCollisionProxy(AActor* owner, UStaticMeshComponent* smc, UStaticMesh* real) {
        if (SimpleShapes(real->BodySetup) <= 0) return false;  // only real simple shapes (see CollisionTick)
        const FTransform world = smc->K2_GetComponentToWorld();
        auto* px = SpawnRooted<UStaticMeshComponent>(owner, world, [&](UStaticMeshComponent* c) { c->SetStaticMesh(real); }, nullptr);
        if (!PtrOk(px)) return false;
        px->SetHiddenInGame(true, false);
        px->SetCollisionEnabled(ECollisionEnabled::QueryAndPhysics);
        px->SetCollisionResponseToAllChannels(ECollisionResponse::ECR_Block);
        g_colDone.insert(ref::Ref(px));
        return true;
    }

    // Rooms: invisible box colliders added to a hub building (see room.hpp).
    struct RoomReq { FVector at; bool zIsFeet; game::RoomInsets inset; float wallHeight; bool teleportHero; };
    SRWLOCK g_roomMu = SRWLOCK_INIT;
    std::vector<RoomReq> g_roomQueue;
    std::atomic<bool> g_roomPending{false};
    std::string g_roomMsg = "no room yet";
    struct RoomBox { ref::Ref holder, box; };  // the spawned actor that owns the box (keeps it from being collected)
    std::unordered_map<ref::Ref, std::vector<RoomBox>, ref::Hash> g_rooms;  // building actor -> its boxes
    std::atomic<bool> g_roomShow{false};  // draw the invisible boxes as outlines (debug)
    bool g_roomShown = false;
    ref::Ref g_roomWorld;

    void SetRoomMsg(const std::string& m) {
        AcquireSRWLockExclusive(&g_roomMu);
        g_roomMsg = m;
        ReleaseSRWLockExclusive(&g_roomMu);
        logger::log("[room] " + m);
    }

    void BuildRoom(const RoomReq& r) {
        UWorld* w = UWorld::GetWorld();
        if (!PtrOk(w)) return;
        if (!g_roomWorld.Is(w)) { g_roomWorld = ref::Ref(w); g_rooms.clear(); }
        // feet = given, or character centre - capsule half height
        float halfHeight = 90.0f;
        AActor* h = HeroActor();
        if (h && h->IsA(ACharacter::StaticClass())) {
            auto* c = static_cast<ACharacter*>(h);
            if (PtrOk(c->CapsuleComponent)) halfHeight = c->CapsuleComponent->CapsuleHalfHeight;
        }
        const float feetZ = r.zIsFeet ? r.at.Z : r.at.Z - halfHeight;
        // the smallest collision-less building whose footprint contains the point
        UClass* smcCls = UStaticMeshComponent::StaticClass();
        AActor* bestActor = nullptr;
        UStaticMeshComponent* best = nullptr;
        float bestArea = 1e30f;
        FVector bMin{}, bMax{};
        for (int li = 0; li < w->Levels.Num(); li++) {
            ULevel* lvl = w->Levels[li];
            if (!PtrOk(lvl)) continue;
            for (int ai = 0; ai < lvl->Actors.Num(); ai++) {
                AActor* a = lvl->Actors[ai];
                if (!PtrOk(a) || !PtrOk(a->RootComponent) || a->IsA(APawn::StaticClass())) continue;
                const FVector p = a->RootComponent->RelativeLocation;
                const float dx = p.X - r.at.X, dy = p.Y - r.at.Y;
                if (dx * dx + dy * dy > 5000.0f * 5000.0f) continue;
                TArray<UActorComponent*> comps = a->K2_GetComponentsByClass(smcCls);
                for (int ci = 0; ci < comps.Num(); ci++) {
                    auto* smc = static_cast<UStaticMeshComponent*>(comps[ci]);
                    if (!PtrOk(smc) || !PtrOk(smc->StaticMesh) || SimpleShapes(smc->StaticMesh->BodySetup) != 0) continue;
                    FVector mn{}, mx{};
                    smc->GetLocalBounds(&mn, &mx);
                    const FVector lp = UKismetMathLibrary::InverseTransformLocation(smc->K2_GetComponentToWorld(), FVector{r.at.X, r.at.Y, feetZ});
                    if (!room::Inside(lp.X, lp.Y, lp.Z, mn.X, mn.Y, mn.Z, mx.X, mx.Y, mx.Z)) continue;
                    const float area = (mx.X - mn.X) * (mx.Y - mn.Y);
                    if (area < bestArea) { bestArea = area; best = smc; bestActor = a; bMin = mn; bMax = mx; }
                }
            }
        }
        if (!best) { SetRoomMsg("no collision-less building around the hero"); return; }
        // A colliding twin of the building (HumanTown/Meshes) beats any box shell: real walls, floors and stairs.
        if (!g_colWorld.Is(w)) { g_colWorld = ref::Ref(w); g_colDone.clear(); g_colLogged.clear(); }
        if (UStaticMesh* real = RealMesh(best->StaticMesh)) {
            if (g_colDone.insert(ref::Ref(best)).second) AddCollisionProxy(bestActor, best, real);
            std::vector<RoomBox>& room = g_rooms[ref::Ref(bestActor)];
            for (const RoomBox& old : room) if (auto* a = old.holder.Get<AActor>()) a->K2_DestroyActor();
            room.clear();
            if (r.teleportHero && h)
                h->K2_SetActorLocation(FVector{r.at.X, r.at.Y, feetZ + halfHeight + 5.0f}, false, nullptr, true);
            SetRoomMsg(best->StaticMesh->GetName() + ": real collision from its twin " + real->GetName());
            return;
        }
        const FTransform t = best->K2_GetComponentToWorld();
        const FVector lf = UKismetMathLibrary::InverseTransformLocation(t, FVector{r.at.X, r.at.Y, feetZ});
        // the local frame may be scaled: convert world-unit inset/thickness/height into local units per axis
        const float sx = std::fabs(t.Scale3D.X) > 1e-3f ? std::fabs(t.Scale3D.X) : 1.0f;
        const float sy = std::fabs(t.Scale3D.Y) > 1e-3f ? std::fabs(t.Scale3D.Y) : 1.0f;
        const float sz = std::fabs(t.Scale3D.Z) > 1e-3f ? std::fabs(t.Scale3D.Z) : 1.0f;
        const float s = std::min(sx, sy);
        std::vector<room::Box> boxes = room::Shell(bMin.X, bMin.Y, bMax.X, bMax.Y, lf.Z,
            {r.inset.minX / sx, r.inset.maxX / sx, r.inset.minY / sy, r.inset.maxY / sy}, 30.0f / s, r.wallHeight / sz);
        // replace this building's previous room
        std::vector<RoomBox>& room = g_rooms[ref::Ref(bestActor)];
        for (const RoomBox& old : room) if (auto* a = old.holder.Get<AActor>()) a->K2_DestroyActor();
        room.clear();
        for (const room::Box& b : boxes) {
            const FVector c = UKismetMathLibrary::TransformLocation(t, FVector{b.cx, b.cy, b.cz});
            FTransform bt = t;
            bt.Translation = c;
            bt.Scale3D = FVector{1.0f, 1.0f, 1.0f};
            AActor* holder = nullptr;
            auto* box = SpawnRooted<UBoxComponent>(bestActor, bt, [](UBoxComponent*) {}, &holder);
            if (!PtrOk(box)) continue;
            box->SetBoxExtent(FVector{b.ex * sx, b.ey * sy, b.ez * sz}, false);
            box->SetCollisionEnabled(ECollisionEnabled::QueryAndPhysics);
            box->SetCollisionResponseToAllChannels(ECollisionResponse::ECR_Block);
            box->SetHiddenInGame(!g_roomShow.load(), false);
            room.push_back({ref::Ref(holder), ref::Ref(box)});
            char bb[200];
            std::snprintf(bb, sizeof(bb), "[room] box at %.0f %.0f %.0f half %.0f %.0f %.0f", c.X, c.Y, c.Z, b.ex * sx, b.ey * sy, b.ez * sz);
            logger::log(bb);
        }
        if (r.teleportHero && h)  // onto the new floor, where the request was made
            h->K2_SetActorLocation(FVector{r.at.X, r.at.Y, feetZ + halfHeight + 5.0f}, false, nullptr, true);
        char buf[300];
        std::snprintf(buf, sizeof(buf), "%s: floor + %d walls at feet z %.0f (building %.0f x %.0f)",
                      best->StaticMesh->GetName().c_str(), int(room.size()) - 1, feetZ,
                      (bMax.X - bMin.X) * sx, (bMax.Y - bMin.Y) * sy);
        SetRoomMsg(buf);
    }

    void RoomTick() {
        std::vector<RoomReq> batch;
        AcquireSRWLockExclusive(&g_roomMu);
        batch.swap(g_roomQueue);
        g_roomPending = false;
        ReleaseSRWLockExclusive(&g_roomMu);
        for (const RoomReq& r : batch) BuildRoom(r);
        const bool show = g_roomShow.load();
        if (show != g_roomShown) {
            g_roomShown = show;
            for (auto& [owner, boxes] : g_rooms)
                for (const RoomBox& b : boxes) if (auto* x = b.box.Get<UBoxComponent>()) x->SetHiddenInGame(!show, false);
        }
    }

    void CollisionTick() {
        std::vector<CollisionReq> batch;
        AcquireSRWLockExclusive(&g_colMu);
        batch.swap(g_colQueue);
        g_colPending = false;
        ReleaseSRWLockExclusive(&g_colMu);
        UWorld* w = UWorld::GetWorld();
        if (!PtrOk(w)) return;
        if (!g_colWorld.Is(w)) { g_colWorld = ref::Ref(w); g_colDone.clear(); g_colLogged.clear(); }
        UClass* smcCls = UStaticMeshComponent::StaticClass();
        for (const CollisionReq& r : batch) {
            int seen = 0, enabled = 0, complex = 0, proxies = 0;
            for (int li = 0; li < w->Levels.Num(); li++) {
                ULevel* lvl = w->Levels[li];
                if (!PtrOk(lvl)) continue;
                for (int ai = 0; ai < lvl->Actors.Num(); ai++) {
                    AActor* a = lvl->Actors[ai];
                    if (!PtrOk(a) || !PtrOk(a->RootComponent) || a->IsA(APawn::StaticClass())) continue;
                    const FVector p = a->RootComponent->RelativeLocation;
                    const float dx = p.X - r.at.X, dy = p.Y - r.at.Y, dz = p.Z - r.at.Z;
                    if (dx * dx + dy * dy + dz * dz > r.radius * r.radius) continue;
                    TArray<UActorComponent*> comps = a->K2_GetComponentsByClass(smcCls);
                    for (int ci = 0; ci < comps.Num(); ci++) {
                        auto* smc = static_cast<UStaticMeshComponent*>(comps[ci]);
                        if (!PtrOk(smc) || g_colDone.count(ref::Ref(smc))) continue;
                        UStaticMesh* mesh = smc->StaticMesh;
                        if (!PtrOk(mesh)) continue;
                        seen++;
                        UBodySetup* bs = mesh->BodySetup;
                        const ECollisionEnabled ce = smc->BodyInstance.CollisionEnabled;
                        const int simple = SimpleShapes(bs);
                        if (g_colLogged.insert(ref::Ref(mesh)).second) {
                            char buf[400];
                            std::snprintf(buf, sizeof(buf), "[collision] %s on %s: enabled=%d simple=%d trace=%d",
                                          mesh->GetName().c_str(), a->GetName().c_str(), int(ce), simple,
                                          PtrOk(bs) ? int(bs->CollisionTraceFlag) : -1);
                            logger::log(buf);
                        }
                        if (!r.fix) continue;
                        g_colDone.insert(ref::Ref(smc));
                        if (simple == 0) {
                            if (UStaticMesh* real = RealMesh(mesh)) {
                                if (AddCollisionProxy(a, smc, real)) proxies++;
                                continue;
                            }
                        }
                        // No shapes = nothing to switch on. Never flip such meshes to "use complex as simple": the hub's
                        // cooked data has no triangle collision, and the shapeless bodies crashed PhysX threads.
                        if (simple <= 0) { complex++; continue; }
                        if (ce == ECollisionEnabled::NoCollision) {
                            smc->SetCollisionEnabled(ECollisionEnabled::NoCollision);  // off → on rebuilds the physics body
                            smc->SetCollisionEnabled(ECollisionEnabled::QueryAndPhysics);
                            smc->SetCollisionResponseToAllChannels(ECollisionResponse::ECR_Block);
                            enabled++;
                        }
                    }
                }
            }
            char buf[200];
            std::snprintf(buf, sizeof(buf), "[collision] %s r=%.0f at %.0f %.0f %.0f: %d new meshes, %d switched on, %d without shapes, %d real-version proxies",
                          r.fix ? "fix" : "scan", r.radius, r.at.X, r.at.Y, r.at.Z, seen, enabled, complex, proxies);
            if (seen) logger::log(buf);
        }
    }

    void PlaceTick() {
        std::vector<Placement> batch;
        AcquireSRWLockExclusive(&g_placeMu);
        batch.swap(g_placeQueue);
        g_placePending = false;
        ReleaseSRWLockExclusive(&g_placeMu);
        for (const Placement& p : batch) {
            auto* npc = p.npc.Get<AActor>();
            if (!npc) continue;
            npc->K2_SetActorLocationAndRotation(p.loc, FRotator{0.0f, p.yaw, 0.0f}, false, nullptr, true);
            if (auto* button = p.button.Get<AActor>()) {
                // click zones are static level actors: the move is silently refused unless they become movable
                if (PtrOk(button->RootComponent)) button->RootComponent->SetMobility(EComponentMobility::Movable);
                const FVector b{p.loc.X + p.offset.X, p.loc.Y + p.offset.Y, p.loc.Z + p.offset.Z};
                button->K2_SetActorLocationAndRotation(b, button->K2_GetActorRotation(), false, nullptr, true);
            }
        }
    }

    void hkProcessEvent(const UObject* obj, UFunction* fn, void* parms) {
        g_inPE++;
        if (g_probeOn.load(std::memory_order_relaxed)) {
            AcquireSRWLockExclusive(&g_probeMu);
            if (const ref::Ref f(fn); g_seen.insert(f).second)
                g_fresh.push_back({f, ref::Ref(PtrOk(obj) ? obj->Class : nullptr), GetTickCount64()});
            ReleaseSRWLockExclusive(&g_probeMu);
        }
        const game::EventFilter skip = g_filter.load(std::memory_order_relaxed);
        if (!skip || !skip(const_cast<UObject*>(obj), fn, parms)) g_oPE(obj, fn, parms);
        for (auto& l : g_listeners)
            if (game::EventListener f = l.load(std::memory_order_relaxed)) f(const_cast<UObject*>(obj), fn, parms);
        if (PtrOk(fn) && PtrOk(parms) && fn->Name.ComparisonIndex == g_camFnName.load(std::memory_order_relaxed)) {
            auto* p = static_cast<Params::PlayerCameraManager_BlueprintUpdateCamera*>(parms);
            if (p->ReturnValue) {  // the game's own pose, captured whether or not it is overridden below
                AcquireSRWLockExclusive(&g_gameCamMu);
                g_gameCam = {p->NewCameraLocation.X, p->NewCameraLocation.Y, p->NewCameraLocation.Z,
                             p->NewCameraRotation.Pitch, p->NewCameraRotation.Yaw, p->NewCameraFOV};
                g_gameCamAt = GetTickCount64();
                ReleaseSRWLockExclusive(&g_gameCamMu);
            }
        }
        if (g_freeOn.load(std::memory_order_relaxed) && PtrOk(fn) && PtrOk(parms)
            && fn->Name.ComparisonIndex == g_camFnName.load(std::memory_order_relaxed)) {
            auto* p = static_cast<Params::PlayerCameraManager_BlueprintUpdateCamera*>(parms);
            AcquireSRWLockShared(&g_freeMu);
            p->NewCameraLocation = FVector{g_freePose.x, g_freePose.y, g_freePose.z};
            p->NewCameraRotation = FRotator{g_freePose.pitch, g_freePose.yaw, 0.0f};
            if (!p->ReturnValue || p->NewCameraFOV < 5.0f) p->NewCameraFOV = 90.0f;  // BP computed nothing: no FOV either
            p->NewCameraFOV += g_freePose.fovDelta;
            ReleaseSRWLockShared(&g_freeMu);
            p->ReturnValue = true;
            g_freeHits++;
        }
        if (!g_inCamMove && GetCurrentThreadId() == g_gameTid.load(std::memory_order_relaxed)) {
            g_inCamMove = true;  // our K2_ calls re-enter ProcessEvent
            if (g_freeOn.load(std::memory_order_relaxed)) {
                const uint32_t seq = g_poseSeq.load(std::memory_order_relaxed);
                if (seq != g_appliedSeq) { g_appliedSeq = seq; MoveViewCam(); }
            } else if (g_camMoved.load(std::memory_order_relaxed)) {
                RestoreViewCam();
            }
            if (g_walkOn.load(std::memory_order_relaxed)) {
                const uint32_t seq = g_walkSeq.load(std::memory_order_relaxed);
                if (seq != g_walkApplied) { g_walkApplied = seq; WalkTick(); }
            } else if (g_walkHeld.load(std::memory_order_relaxed)) {
                WalkRelease();
            }
            if (g_placePending.load(std::memory_order_relaxed)) PlaceTick();
            if (g_colPending.load(std::memory_order_relaxed)) CollisionTick();
            if (g_roomPending.load(std::memory_order_relaxed)) RoomTick();
            g_inCamMove = false;
        }
        g_inPE--;
    }

    // One ProcessEvent hook shared by the probe, event listeners, free camera, hub walk, NPC placement and rooms:
    // installed while any needs it (including until a moved view camera / possessed hero is given back).
    void UpdatePEHook() {
        bool want = g_probeOn.load() || g_freeOn.load() || g_camMoved.load() || g_walkOn.load()
                 || g_walkHeld.load() || g_placePending.load() || g_colPending.load() || g_roomPending.load();
        for (auto& l : g_listeners) want |= l.load() != nullptr;
        want |= g_filter.load() != nullptr;
        if (want && !g_peTarget) {
            MH_Initialize();  // already initialised by kiero: harmless
            void* target = reinterpret_cast<void*>(InSDKUtils::GetImageBase() + Offsets::ProcessEvent);
            if (MH_CreateHook(target, (void*)hkProcessEvent, (void**)&g_oPE) != MH_OK || MH_EnableHook(target) != MH_OK) {
                logger::log("[hook] ProcessEvent hook failed");
                return;
            }
            g_peTarget = target;
            logger::log("[hook] ProcessEvent hooked");
        }
        if (!want && g_peTarget) {
            MH_DisableHook(g_peTarget);
            MH_RemoveHook(g_peTarget);
            g_peTarget = nullptr;
            for (int i = 0; i < 200 && g_inPE.load() > 0; i++) Sleep(10);  // let in-flight calls leave our code before unload
            logger::log("[hook] ProcessEvent unhooked");
        }
    }
}

void game::SetEventFilter(EventFilter f, bool on) {
    EventFilter want = on ? nullptr : f;
    if (!g_filter.compare_exchange_strong(want, on ? f : nullptr) && on && want != f) logger::log("[game] event filter taken: not installed");
    if (!on) for (int i = 0; i < 200 && g_inPE.load() > 0; i++) Sleep(10);  // in-flight calls may still be inside f
    UpdatePEHook();
}

void game::SetEventProbe(bool on) {
    g_probeOn = on;
    UpdatePEHook();
}

void game::SetEventListener(EventListener l, bool on) {
    bool have = false;
    for (auto& s : g_listeners) {
        if (s.load() != l) continue;
        if (on) have = true; else s = nullptr;
    }
    for (auto& s : g_listeners)
        if (on && !have && !s.load()) { s = l; have = true; }
    if (on && !have) logger::log("[game] event listener table full: a feature gets no game events");
    if (!on) for (int i = 0; i < 200 && g_inPE.load() > 0; i++) Sleep(10);  // in-flight calls may still be inside l
    UpdatePEHook();
}

namespace {
    // Two users of the camera override (render thread only): the higher priority wins while it is set.
    bool g_slotOn[2] = {};
}

namespace {
    bool CamFnKnown() {
        if (g_camFnName.load() >= 0) return true;
        UFunction* fn = APlayerCameraManager::StaticClass()->GetFunction("PlayerCameraManager", "BlueprintUpdateCamera");
        if (!PtrOk(fn)) return false;
        g_camFnName = fn->Name.ComparisonIndex;
        return true;
    }
}

bool game::SetFreeCam(const CamPose* pose, int priority) {
    const int me = priority > 0 ? 1 : 0, other = 1 - me;
    if (pose) {
        if (!CamFnKnown() || !EnsureGameTid()) return false;  // the game window has no focus yet
        g_slotOn[me] = true;
        if (g_slotOn[other] && other > me) return false;  // outranked: the other user's pose stays on screen
        AcquireSRWLockExclusive(&g_freeMu);
        g_freePose = *pose;
        ReleaseSRWLockExclusive(&g_freeMu);
        g_poseSeq++;
        if (!g_freeOn.exchange(true)) UpdatePEHook();
        return true;
    }
    if (!g_slotOn[me]) return false;
    g_slotOn[me] = false;
    if (g_slotOn[other]) return false;  // the other user keeps the camera (it posts its pose every frame)
    g_freeOn = false;
    // The game thread puts a moved view camera back on its next ProcessEvent; wait briefly, then unhook regardless.
    for (int i = 0; i < 50 && g_camMoved.load(); i++) Sleep(2);
    if (g_camMoved.exchange(false)) logger::log("[freecam] view camera not restored in time");
    UpdatePEHook();
    return false;
}

int game::FreeCamOverrides() { return g_freeHits.load(); }

bool game::OnGameThread() { return EnsureGameTid() && GetCurrentThreadId() == g_gameTid.load(); }

bool game::GameCamPose(CamPose& out) {
    CamFnKnown();  // the hook captures the game's pose once it knows the function's name
    AcquireSRWLockShared(&g_gameCamMu);
    out = g_gameCam;
    const bool fresh = g_gameCamAt && GetTickCount64() - g_gameCamAt < 250;
    ReleaseSRWLockShared(&g_gameCamMu);
    return fresh;
}

int game::CamOwner() { return g_slotOn[1] ? 1 : g_slotOn[0] ? 0 : -1; }

namespace {
    // Render-thread class-name cache: hub blueprints aren't in the SDK, so they are told apart by name.
    enum Kind { kOther, kNpc, kButton, kHero };
    std::unordered_map<ref::Ref, Kind, ref::Hash> g_kinds;  // by UClass
    std::unordered_map<ref::Ref, std::string, ref::Hash> g_npcNames;
    struct Pair { ref::Ref button; FVector offset; };  // AActor
    std::unordered_map<ref::Ref, Pair, ref::Hash> g_pairs;  // by NPC actor. ponytail: never pruned (13 NPCs)

    Kind KindOf(AActor* a) {
        UClass* c = a->Class;
        const ref::Ref key(c);
        auto it = g_kinds.find(key);
        if (it != g_kinds.end()) return it->second;
        const std::string n = c->GetName();
        const bool character = a->IsA(ACharacter::StaticClass());
        Kind k = kOther;
        if (character && n.rfind("NPC_", 0) == 0) {
            k = kNpc;
            std::string s = n.substr(4);
            if (s.size() > 2 && s.compare(s.size() - 2, 2, "_C") == 0) s.resize(s.size() - 2);
            g_npcNames[key] = s;
        } else if (n == "BP_TriggerVolumeButton_Character_C") k = kButton;
        else if (character && n.find("_Player_C") != std::string::npos) k = kHero;
        g_kinds[key] = k;
        return k;
    }

    template <class F> void ForEachActor(F&& fn) {
        UWorld* w = UWorld::GetWorld();
        if (!PtrOk(w)) return;
        for (int li = 0; li < w->Levels.Num(); li++) {
            ULevel* lvl = w->Levels[li];
            if (!PtrOk(lvl)) continue;
            for (int ai = 0; ai < lvl->Actors.Num(); ai++) {
                AActor* a = lvl->Actors[ai];
                if (PtrOk(a) && PtrOk(a->Class) && PtrOk(a->RootComponent)) fn(a);
            }
        }
    }

    FVector Loc(AActor* a) { return a->RootComponent->RelativeLocation; }  // unattached root: relative == world
}

game::Hero game::HubHero() {
    Hero out;
    AActor* hero = HeroActor();
    if (!hero || !PtrOk(hero->RootComponent)) {
        hero = nullptr;
        ForEachActor([&](AActor* a) { if (!hero && KindOf(a) == kHero) hero = a; });
        AcquireSRWLockExclusive(&g_walkMu);
        g_hero = ref::Ref(hero);
        ReleaseSRWLockExclusive(&g_walkMu);
    }
    if (!hero) return out;
    APlayerController* pc = LocalPC();
    const FVector p = Loc(hero);
    out = {true, PtrOk(pc) && pc->Pawn == hero, p.X, p.Y, p.Z, hero->RootComponent->RelativeRotation.Yaw};
    if (hero->IsA(ACharacter::StaticClass())) {
        auto* c = static_cast<ACharacter*>(hero);
        if (PtrOk(c->CharacterMovement)) out.moveMode = int(c->CharacterMovement->MovementMode);
    }
    out.possessTries = g_possessTries.load();
    out.modeFixes = g_modeFixes.load();
    return out;
}

void game::SetHubWalk(const WalkInput* in) {
    if (in) {
        if (!EnsureGameTid()) return;
        AcquireSRWLockExclusive(&g_walkMu);
        g_walkIn = *in;
        ReleaseSRWLockExclusive(&g_walkMu);
        g_walkSeq++;
        if (!g_walkOn.exchange(true)) UpdatePEHook();
        return;
    }
    if (!g_walkOn.exchange(false) && !g_walkHeld.load()) return;
    for (int i = 0; i < 50 && g_walkHeld.load(); i++) Sleep(2);  // game thread gives the pawn back on its next event
    if (g_walkHeld.exchange(false)) logger::log("[walk] possession not restored in time");
    UpdatePEHook();
}

std::vector<game::Npc> game::ListNpcs() {
    std::vector<AActor*> npcs, buttons;
    ForEachActor([&](AActor* a) {
        const Kind k = KindOf(a);
        if (k == kNpc) npcs.push_back(a);
        else if (k == kButton) buttons.push_back(a);
    });
    std::vector<Npc> out;
    for (AActor* n : npcs) {
        const FVector p = Loc(n);
        const ref::Ref key(n);
        if (!g_pairs.count(key)) {  // first sight = vanilla spot: pair with the nearest click zone
            AActor* best = nullptr;
            float bestD = 500.0f * 500.0f;
            for (AActor* b : buttons) {
                const FVector q = Loc(b);
                const float d = (q.X - p.X) * (q.X - p.X) + (q.Y - p.Y) * (q.Y - p.Y) + (q.Z - p.Z) * (q.Z - p.Z);
                if (d < bestD) { bestD = d; best = b; }
            }
            g_pairs[key] = {ref::Ref(best), best ? FVector{Loc(best).X - p.X, Loc(best).Y - p.Y, Loc(best).Z - p.Z} : FVector{}};
        }
        out.push_back({reinterpret_cast<uintptr_t>(n), g_npcNames[ref::Ref(n->Class)], p.X, p.Y, p.Z,
                       n->RootComponent->RelativeRotation.Yaw, g_pairs[key].button.ptr != nullptr});
    }
    if (!g_placePending.load() && !g_colPending.load() && !g_roomPending.load()) UpdatePEHook();  // drop the hook once queued work is done
    return out;
}

void game::MakeRoom(float x, float y, float z, bool zIsFeet, const RoomInsets& inset, float wallHeight, bool teleportHero) {
    if (!EnsureGameTid()) return;
    AcquireSRWLockExclusive(&g_roomMu);
    g_roomQueue.push_back({FVector{x, y, z}, zIsFeet, inset, wallHeight, teleportHero});
    g_roomPending = true;
    ReleaseSRWLockExclusive(&g_roomMu);
    UpdatePEHook();
}

void game::ShowRoom(bool show) {
    if (g_roomShow.exchange(show) == show || !EnsureGameTid()) return;
    g_roomPending = true;  // RoomTick applies it
    UpdatePEHook();
}

std::string game::RoomStatus() {
    AcquireSRWLockShared(&g_roomMu);
    std::string m = g_roomMsg;
    ReleaseSRWLockShared(&g_roomMu);
    return m;
}

void game::FixCollision(float x, float y, float z, float radius, bool fix) {
    if (!EnsureGameTid()) return;
    AcquireSRWLockExclusive(&g_colMu);
    g_colQueue.push_back({FVector{x, y, z}, radius, fix});
    g_colPending = true;
    ReleaseSRWLockExclusive(&g_colMu);
    UpdatePEHook();
}

void game::PlaceNpc(uintptr_t id, float x, float y, float z, float yaw) {
    // id = the address ListNpcs handed out; never dereferenced here, the game thread checks the Ref
    auto it = std::find_if(g_pairs.begin(), g_pairs.end(), [&](const auto& kv) { return reinterpret_cast<uintptr_t>(kv.first.ptr) == id; });
    if (it == g_pairs.end() || !EnsureGameTid()) return;
    AcquireSRWLockExclusive(&g_placeMu);
    g_placeQueue.push_back({it->first, it->second.button, it->second.offset, FVector{x, y, z}, yaw});
    g_placePending = true;
    ReleaseSRWLockExclusive(&g_placeMu);
    UpdatePEHook();
}

int game::LogActors() {
    UWorld* w = UWorld::GetWorld();
    if (!PtrOk(w)) return 0;
    int n = 0;
    for (int li = 0; li < w->Levels.Num(); li++) {
        ULevel* lvl = w->Levels[li];
        if (!PtrOk(lvl)) continue;
        for (int ai = 0; ai < lvl->Actors.Num(); ai++) {
            AActor* a = lvl->Actors[ai];
            if (!PtrOk(a) || !PtrOk(a->Class)) continue;
            const USceneComponent* root = a->RootComponent;
            const FVector p = PtrOk(root) ? root->RelativeLocation : FVector{};
            char buf[512];
            std::snprintf(buf, sizeof(buf), "[actors] L%d %s | %s @ %.0f %.0f %.0f", li, a->Class->GetName().c_str(),
                          a->GetName().c_str(), p.X, p.Y, p.Z);
            logger::log(buf);
            n++;
        }
    }
    return n;
}

void game::CameraClasses(std::string& manager, std::string& target) {
    manager = target = "-";
    APlayerController* pc = LocalPC();
    if (!PtrOk(pc)) return;
    APlayerCameraManager* cam = pc->PlayerCameraManager;
    if (!PtrOk(cam) || !PtrOk(cam->Class)) return;
    manager = cam->Class->GetName();
    AActor* t = cam->ViewTarget.Target;
    if (PtrOk(t) && PtrOk(t->Class)) target = t->Class->GetName();
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
        const UFunction* fn = f.fn.Get<UFunction>();
        const UClass* cls = f.cls.Get<UClass>();
        std::snprintf(buf, sizeof(buf), "[probe] t=%llu fn=%s on=%s", f.t,
                      fn ? fn->GetFullName().c_str() : "?", cls ? cls->GetName().c_str() : "?");
        logger::log(buf);
    }
}

namespace {
    // Same call shape as Dumper-7's generated bodies (GameplayAbilities_functions.cpp isn't linked).
    void CallFn(const UObject* obj, UFunction* fn, void* parms) {
        const auto flags = fn->FunctionFlags;
        fn->FunctionFlags |= 0x400;  // FUNC_Native
        obj->ProcessEvent(fn, parms);
        fn->FunctionFlags = flags;
    }

    // The game exe's own address range (vtables live there, heap objects don't).
    bool InImage(uintptr_t p) {
        static const uintptr_t base = InSDKUtils::GetImageBase();
        static const uintptr_t size = reinterpret_cast<const IMAGE_NT_HEADERS*>(
            base + reinterpret_cast<const IMAGE_DOS_HEADER*>(base)->e_lfanew)->OptionalHeader.SizeOfImage;
        return p >= base && p < base + size;
    }

    // FGameplayEffectContextHandle is 0x18 here: a TSharedPtr<FGameplayEffectContext>, behind a vtable
    // pointer if the first word points into the exe. Context: AbilityCDO @0x18, AbilityInstanceNotReplicated @0x20.
    const uint8* EffectContext(const FGameplayEffectContextHandle& h) {
        const uintptr_t* q = reinterpret_cast<const uintptr_t*>(&h);
        const uintptr_t ctx = InImage(q[0]) ? q[1] : q[0];
        return PtrOk(reinterpret_cast<void*>(ctx)) ? reinterpret_cast<const uint8*>(ctx) : nullptr;
    }
}

int game::CooldownEffects(void* ascp, void* abp, EffectRef* out, int max) {
    auto* asc = static_cast<UAbilitySystemComponent*>(ascp);
    auto* ab = static_cast<UGameplayAbility*>(abp);
    if (!PtrOk(asc) || !PtrOk(ab)) return 0;
    UClass* cd = ab->CooldownGameplayEffectClass.Get();
    if (ab->IsA(UArchonGameplayAbility::StaticClass())) {
        static ref::Fn getCd{UArchonGameplayAbility::StaticClass, "ArchonGameplayAbility", "GetCooldownGEClass"};
        UFunction* fn = getCd.Get();
        if (!fn) return 0;
        Params::ArchonGameplayAbility_GetCooldownGEClass p{};
        CallFn(ab, fn, &p);
        cd = p.ReturnValue.Get();
    }
    if (!PtrOk(cd)) return 0;
    const int32 cdoIdx = PtrOk(ab->Class->ClassDefaultObject) ? ab->Class->ClassDefaultObject->Index : -1;

    int n = 0;
    auto& effects = asc->ActiveGameplayEffects.GameplayEffects_Internal;
    for (int i = 0; i < effects.Num() && n < max; i++) {
        const FActiveGameplayEffect& e = effects[i];
        if (!PtrOk(e.Spec.Def) || !e.Spec.Def->IsA(cd)) continue;
        const uint8* ctx = EffectContext(e.Spec.EffectContext);
        if (!ctx) continue;
        const int32 byCdo = *reinterpret_cast<const int32*>(ctx + 0x18);
        const int32 byInstance = *reinterpret_cast<const int32*>(ctx + 0x20);
        if (byCdo != cdoIdx && byInstance != ab->Index) continue;
        // FActiveGameplayEffect::Handle sits after the 12-byte FFastArraySerializerItem (Dumper-7 Pad_C).
        const int32 h = *reinterpret_cast<const int32*>(reinterpret_cast<const uint8*>(&e) + 0x0C);
        if (h > 0) out[n++] = {h, e.Spec.Duration};
    }
    return n;
}

bool game::RemoveEffect(void* ascp, int handle) {
    auto* asc = static_cast<UAbilitySystemComponent*>(ascp);
    static ref::Fn remove{UAbilitySystemComponent::StaticClass, "AbilitySystemComponent", "RemoveActiveGameplayEffect"};
    UFunction* fn = remove.Get();
    if (!PtrOk(asc) || !fn || handle <= 0) return false;
    Params::AbilitySystemComponent_RemoveActiveGameplayEffect p{};
    *reinterpret_cast<int32*>(&p.Handle) = handle;
    p.StacksToRemove = -1;
    CallFn(asc, fn, &p);
    return p.ReturnValue;
}

namespace game {
    void* FindSingleton(const char* className, const char* objectName) {
        LARGE_INTEGER t0, t1, f;
        QueryPerformanceCounter(&t0);
        UObject* found = nullptr;
        for (int i = 0; !found && UObject::GObjects && i < UObject::GObjects->Num(); i++) {
            UObject* o = UObject::GObjects->GetByIndex(i);
            if (PtrOk(o) && PtrOk(o->Class) && !o->IsDefaultObject() && o->Class->GetName() == className &&
                (!objectName || o->GetName() == objectName))
                found = o;
        }
        QueryPerformanceCounter(&t1);
        QueryPerformanceFrequency(&f);
        char buf[160];
        std::snprintf(buf, sizeof buf, "[game] singleton %s %s: %s, %.2f ms", className, objectName ? objectName : "", found ? "found" : "none",
                      double(t1.QuadPart - t0.QuadPart) * 1000.0 / double(f.QuadPart));
        logger::log(buf);
        return found;
    }
}

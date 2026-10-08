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
#include "BP_GameAbilityBase_classes.hpp"        // ability dump: montage override maps
#include "BP_GameAbility_WeaponMontage_classes.hpp"
#include "BP_GameplayAnimNotify_classes.hpp"     // ability dump: notify type

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
        static std::unordered_map<UClass*, combat::Element> cache;
        if (!type) return combat::Element::Physical;
        auto it = cache.find(type);
        if (it == cache.end()) it = cache.emplace(type, combat::Classify(type->GetName())).first;
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
    std::atomic<game::EventListener> g_listeners[4] = {};

    void hkProcessEvent(const UObject* obj, UFunction* fn, void* parms) {
        g_inPE++;
        if (g_probeOn.load(std::memory_order_relaxed)) {
            AcquireSRWLockExclusive(&g_probeMu);
            if (g_seen.insert(fn).second)
                g_fresh.push_back({fn, PtrOk(obj) ? obj->Class : nullptr, GetTickCount64()});
            ReleaseSRWLockExclusive(&g_probeMu);
        }
        g_oPE(obj, fn, parms);
        for (auto& l : g_listeners)
            if (game::EventListener f = l.load(std::memory_order_relaxed)) f(const_cast<UObject*>(obj), fn, parms);
        g_inPE--;
    }

    // Installed while the probe or any listener needs it.
    void UpdateHook() {
        bool need = g_probeOn.load();
        for (auto& l : g_listeners) need |= l.load() != nullptr;
        if (need && !g_peTarget) {
            MH_Initialize();  // already initialised by kiero: harmless
            void* target = reinterpret_cast<void*>(InSDKUtils::GetImageBase() + Offsets::ProcessEvent);
            if (MH_CreateHook(target, (void*)hkProcessEvent, (void**)&g_oPE) != MH_OK || MH_EnableHook(target) != MH_OK) {
                logger::log("[probe] ProcessEvent hook failed");
                return;
            }
            g_peTarget = target;
            logger::log("[probe] ProcessEvent hooked");
        }
        if (!need && g_peTarget) {
            MH_DisableHook(g_peTarget);
            MH_RemoveHook(g_peTarget);
            g_peTarget = nullptr;
            for (int i = 0; i < 200 && g_inPE.load() > 0; i++) Sleep(10);  // let in-flight calls leave our code before unload
        }
    }
}

void game::SetEventProbe(bool on) {
    g_probeOn = on;
    UpdateHook();
}

void game::SetEventListener(EventListener l, bool on) {
    bool have = false;
    for (auto& s : g_listeners) {
        if (s.load() != l) continue;
        if (on) have = true; else s = nullptr;
    }
    for (auto& s : g_listeners)
        if (on && !have && !s.load()) { s = l; have = true; }
    if (!on) for (int i = 0; i < 200 && g_inPE.load() > 0; i++) Sleep(10);  // in-flight calls may still be inside l
    UpdateHook();
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

// ---- Ability dump (appended; see features/ability-dump) ----
namespace {
    using namespace ability_dump;

    Montage ReadMontage(UAnimMontage* m, const std::string& path) {
        Montage out;
        out.path = path;
        if (!PtrOk(m)) return out;
        out.loaded = true;
        out.length = m->SequenceLength;
        UClass* notifyCls = UBP_GameplayAnimNotify_C::StaticClass();
        auto& ns = m->Notifies;
        if (ns.Num() < 0 || ns.Num() > 4096) return out;
        for (int i = 0; i < ns.Num(); i++) {
            UAnimNotify* n = ns[i].Notify;
            if (PtrOk(n) && n->IsA(notifyCls))
                out.notifies.Add(static_cast<int>(static_cast<UBP_GameplayAnimNotify_C*>(n)->mGameplayAnimNotifyType));
            else out.notifies.other++;
        }
        return out;
    }

    void AddSoft(const TSoftObjectPtr<UAnimMontage>& sp, std::vector<Montage>& out) {
        std::string path = sp.ObjectID.AssetPathName.ToString();
        if (path.empty() || path == "None") return;
        for (const Montage& m : out) if (m.path == path) return;
        UObject* o = sp.Get();  // memory read via GObjects index
        out.push_back(ReadMontage(PtrOk(o) && o->IsA(UAnimMontage::StaticClass()) ? static_cast<UAnimMontage*>(o) : nullptr, path));
    }

    // The SDK's TMap accessors don't compile (SetElement::Value is private), so read the sparse array raw:
    // Data pointer at +0, element = pair + {HashNextId, HashIndex}, free slots skipped via IsValidIndex.
    template <class Map, class Fn> void EachPair(Map& map, Fn&& fn) {
        using P = typename Map::ElementType;
        if (!map.IsValid() || map.Num() > 256 || map.NumAllocated() > 1024) return;
        const uintptr_t stride = (sizeof(P) + 8 + alignof(P) - 1) & ~(uintptr_t)(alignof(P) - 1);
        const uint8_t* data = *reinterpret_cast<uint8_t* const*>(&map);
        if (!PtrOk(data)) return;
        for (int i = 0; i < map.NumAllocated(); i++)
            if (map.IsValidIndex(i)) fn(*reinterpret_cast<const P*>(data + i * stride));
    }

    static_assert(sizeof(TSoftObjectPtr<UAnimMontage>) == 0x28, "soft ptr = weak 8 + tag 4(+4) + path 0x18");
    static_assert(sizeof(UC::TPair<EWeaponType, TSoftObjectPtr<UAnimMontage>>) == 0x30, "TMap element stride (+8 hash) = 0x38");

    template <class Map> void AddMap(Map& map, std::vector<Montage>& out) {
        EachPair(map, [&](const auto& kv) { AddSoft(kv.Second, out); });
    }

    template <class Map> void AddNested(Map& map, std::vector<Montage>& out) {
        EachPair(map, [&](const auto& kv) { AddMap(const_cast<FSWeaponTypeMontageMap&>(kv.Second).Map_6_BBEFCEAE4A11569EBD77B58B5B29E761, out); });
    }

    Magnitude ReadMagnitude(UClass* ge) {
        Magnitude m;
        if (!PtrOk(ge)) return m;
        m.geClass = ge->GetName();
        auto* cdo = static_cast<UGameplayEffect*>(ge->ClassDefaultObject);
        if (!PtrOk(cdo)) return m;
        const auto& dm = cdo->DurationMagnitude;
        m.calcType = static_cast<int>(dm.MagnitudeCalculationType);
        m.value = dm.ScalableFloatMagnitude.Value;
        const std::string row = dm.ScalableFloatMagnitude.Curve.RowName.ToString();
        if (row != "None") m.curveRow = row;
        return m;
    }

    void ReadTags(const FGameplayTagContainer& c, std::vector<std::string>& out) {
        if (c.GameplayTags.Num() < 0 || c.GameplayTags.Num() > 256) return;
        for (int i = 0; i < c.GameplayTags.Num(); i++) out.push_back(c.GameplayTags[i].TagName.ToString());
    }
}

void game::DumpAbilities(std::vector<ability_dump::Ability>& out, ability_dump::Live& live) {
    UClass* abilityCls = UArchonGameplayAbility::StaticClass();
    UClass* baseCls = UBP_GameAbilityBase_C::StaticClass();
    UClass* weaponCls = UBP_GameAbility_WeaponMontage_C::StaticClass();
    if (UObject::GObjects) {
        const int n = UObject::GObjects->Num();
        for (int i = 0; i < n; i++) {
            UObject* o = UObject::GObjects->GetByIndex(i);
            if (!PtrOk(o) || !(static_cast<uint32>(o->Flags) & 0x10) || !PtrOk(o->Class) || !o->IsA(abilityCls)) continue;
            auto* a = static_cast<UArchonGameplayAbility*>(o);
            Ability r;
            r.cls = o->Class->GetName();
            ReadTags(a->AbilityTags, r.tags);
            r.cooldown = ReadMagnitude(a->CooldownGameplayEffectClass.Get());
            r.castTime = ReadMagnitude(a->mCastTimeGEclass.Get());
            if (o->IsA(baseCls)) {
                auto* b = static_cast<UBP_GameAbilityBase_C*>(o);
                AddNested(b->AnimSkeleton_OverrideWeaponAnims, r.montages);
                AddMap(b->OverrideAnims.Map_4_A6AB669E4D8409B5B109D0B88C908487, r.montages);
            }
            if (o->IsA(weaponCls)) {
                auto* w = static_cast<UBP_GameAbility_WeaponMontage_C*>(o);
                AddNested(w->OverrideWeaponAnimsLeft, r.montages);
                AddNested(w->OverrideWeaponAnimsRight, r.montages);
            }
            out.push_back(std::move(r));
        }
    }
    live = Live{};
    APlayerController* pc = LocalPC();
    APawn* pawn = PtrOk(pc) ? pc->Pawn : nullptr;
    if (!PtrOk(pawn) || !pawn->IsA(AArchonCharacter::StaticClass())) return;
    UArchonAbilitySystemComponent* asc = static_cast<AArchonCharacter*>(pawn)->mAbilitySystemComponent;
    if (!PtrOk(asc)) return;
    const FGameplayAbilityLocalAnimMontage& li = asc->LocalAnimMontageInfo;
    if (!PtrOk(li.AnimMontage) || !li.AnimMontage->IsA(UAnimMontage::StaticClass())) return;
    live.valid = true;
    live.ability = PtrOk(li.AnimatingAbility) && PtrOk(li.AnimatingAbility->Class) ? li.AnimatingAbility->Class->GetName() : "?";
    live.montage = ReadMontage(static_cast<UAnimMontage*>(li.AnimMontage), li.AnimMontage->GetName());
}

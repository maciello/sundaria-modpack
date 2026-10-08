#include "props.hpp"
#include "game.hpp"
#include "logger.hpp"
#include "ref.hpp"

#include <Windows.h>
#include <atomic>
#include <cmath>
#include <unordered_map>
#include "Engine_classes.hpp"

using namespace SDK;

namespace {
    bool PtrOk(const void* p) {
        const uintptr_t v = reinterpret_cast<uintptr_t>(p);
        return v > 0x10000 && v < 0x7FFFFFFFFFFFull;
    }

    // render thread → game thread (latest request per kind wins; moves are posted every frame while carrying)
    struct Req { std::string name; bool highlight; bool on; int stencil; FVector at; float yaw; bool carrying; };
    SRWLOCK g_mu = SRWLOCK_INIT;
    std::vector<Req> g_queue;
    std::atomic<bool> g_pending{false}, g_listening{false};

    // render thread → game thread: the last Scan request and its answer, guarded by g_mu
    struct NearReq { float x, y, z, radius; };
    NearReq g_scan{};
    bool g_scanAsked = false;
    std::vector<props::Prop> g_near;
    std::vector<props::Prop> NearNow(const NearReq& q);

    // game thread: name → actor, resolved once per map
    std::unordered_map<std::string, ref::Ref> g_byName;
    ref::Ref g_world;

    AActor* Find(const std::string& name) {
        UWorld* w = UWorld::GetWorld();
        if (!PtrOk(w)) return nullptr;
        if (!g_world.Is(w)) { g_world = ref::Ref(w); g_byName.clear(); }
        auto it = g_byName.find(name);
        if (it != g_byName.end()) return it->second.Get<AActor>();
        for (int li = 0; li < w->Levels.Num(); li++) {  // once per prop per map
            ULevel* lvl = w->Levels[li];
            if (!PtrOk(lvl)) continue;
            for (int ai = 0; ai < lvl->Actors.Num(); ai++) {
                AActor* a = lvl->Actors[ai];
                if (PtrOk(a) && a->GetName() == name) { g_byName[name] = ref::Ref(a); return a; }
            }
        }
        g_byName[name] = ref::Ref();
        return nullptr;
    }

    UStaticMeshComponent* MeshOf(AActor* a) {
        if (!a->IsA(AStaticMeshActor::StaticClass())) return nullptr;
        UStaticMeshComponent* c = static_cast<AStaticMeshActor*>(a)->StaticMeshComponent;
        return PtrOk(c) ? c : nullptr;
    }

    void Run(const Req& r) {
        AActor* a = Find(r.name);
        if (!a) return;
        UStaticMeshComponent* c = MeshOf(a);
        if (r.highlight) {
            if (!c) return;
            c->SetCustomDepthStencilValue(r.stencil);
            c->SetRenderCustomDepth(r.on);
            return;
        }
        if (PtrOk(a->RootComponent)) a->RootComponent->SetMobility(EComponentMobility::Movable);  // level props are static
        a->SetActorEnableCollision(!r.carrying);  // carried: don't shove the hero around
        const FRotator rot = PtrOk(a->RootComponent) ? a->RootComponent->RelativeRotation : FRotator{};
        a->K2_SetActorLocationAndRotation(r.at, FRotator{rot.Pitch, r.yaw, rot.Roll}, false, nullptr, true);
    }

    void OnEvent(void*, void*, void*) {
        if (!game::OnGameThread() || !g_pending.exchange(false)) return;
        std::vector<Req> batch;
        AcquireSRWLockExclusive(&g_mu);
        batch.swap(g_queue);
        const bool scan = g_scanAsked;
        const NearReq q = g_scan;
        g_scanAsked = false;
        ReleaseSRWLockExclusive(&g_mu);
        for (const Req& r : batch) Run(r);
        if (!scan) return;
        std::vector<props::Prop> found = NearNow(q);
        AcquireSRWLockExclusive(&g_mu);
        g_near.swap(found);
        ReleaseSRWLockExclusive(&g_mu);
    }

    void Wake() {
        g_pending = true;
        if (!g_listening.exchange(true)) game::SetEventListener(OnEvent, true);
    }

    void Post(Req r) {
        AcquireSRWLockExclusive(&g_mu);
        if (!r.highlight)  // only the newest move of a prop matters
            for (Req& q : g_queue) if (!q.highlight && q.name == r.name) { q = r; r.name.clear(); break; }
        if (!r.name.empty()) g_queue.push_back(r);
        ReleaseSRWLockExclusive(&g_mu);
        Wake();
    }

// Game thread. ponytail: walks the loaded levels' actors; build mode asks only after the hero moved 2 m
std::vector<props::Prop> NearNow(const NearReq& q) {
    using props::Prop;
    const float x = q.x, y = q.y, z = q.z, radius = q.radius;
    std::vector<Prop> out;
    UWorld* w = UWorld::GetWorld();
    if (!PtrOk(w)) return out;
    UClass* smaCls = AStaticMeshActor::StaticClass();
    for (int li = 0; li < w->Levels.Num(); li++) {
        ULevel* lvl = w->Levels[li];
        if (!PtrOk(lvl)) continue;
        for (int ai = 0; ai < lvl->Actors.Num(); ai++) {
            AActor* a = lvl->Actors[ai];
            if (!PtrOk(a) || !PtrOk(a->RootComponent) || !a->IsA(smaCls)) continue;
            const FVector p = a->RootComponent->RelativeLocation;
            const float dx = p.X - x, dy = p.Y - y, dz = p.Z - z;
            if (dx * dx + dy * dy + dz * dz > radius * radius) continue;
            UStaticMeshComponent* c = static_cast<AStaticMeshActor*>(a)->StaticMeshComponent;
            if (!PtrOk(c) || !PtrOk(c->StaticMesh)) continue;  // shapeless props too (the anvil has no collision)
            const FBoxSphereBounds& b = c->StaticMesh->ExtendedBounds;
            const FVector s = a->RootComponent->RelativeScale3D;
            const float scale = std::fmax(std::fabs(s.X), std::fmax(std::fabs(s.Y), std::fabs(s.Z)));
            const float r = b.SphereRadius * scale;
            if (r > 250.0f || r < 10.0f) continue;  // walls/houses or crumbs
            const float base = (b.BoxExtent.Z - b.Origin.Z) * std::fabs(s.Z);  // pivot above the bottom
            out.push_back({a->GetName(), p.X, p.Y, p.Z, p.Z + b.Origin.Z * s.Z, a->RootComponent->RelativeRotation.Yaw, r, base});
        }
    }
    return out;
}
}

void props::Scan(float x, float y, float z, float radius) {
    AcquireSRWLockExclusive(&g_mu);
    g_scan = {x, y, z, radius};
    g_scanAsked = true;
    ReleaseSRWLockExclusive(&g_mu);
    Wake();
}

std::vector<props::Prop> props::Near() {
    AcquireSRWLockShared(&g_mu);
    std::vector<Prop> out = g_near;
    ReleaseSRWLockShared(&g_mu);
    return out;
}

void props::Highlight(const std::string& name, bool on, int stencil) { Post({name, true, on, stencil, {}, 0, false}); }

void props::Move(const std::string& name, float x, float y, float z, float yaw, bool carrying) {
    Post({name, false, false, 0, FVector{x, y, z}, yaw, carrying});
}

void props::Stop() {
    for (int i = 0; i < 50 && g_pending.load(); i++) Sleep(2);
    if (g_listening.exchange(false)) game::SetEventListener(OnEvent, false);
}

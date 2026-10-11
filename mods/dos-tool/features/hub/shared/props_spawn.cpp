#include "props_spawn.hpp"
#include "game.hpp"
#include "logger.hpp"
#include "ref.hpp"

#include <Windows.h>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <set>
#include <unordered_map>
#include "Engine_classes.hpp"

using namespace SDK;

namespace {
    bool PtrOk(const void* p) {
        const uintptr_t v = reinterpret_cast<uintptr_t>(p);
        return v > 0x10000 && v < 0x7FFFFFFFFFFFull;
    }

    std::string PathOf(UObject* o) {  // the package's raw FName keeps the folders, GetName() drops them
        std::string pkg;
        for (UObject* p = o->Outer; PtrOk(p); p = p->Outer) pkg = p->Name.GetRawString();
        return pkg + "." + o->GetName();
    }

    // render thread → game thread
    struct Req { int id; bool remove; std::string mesh; FVector at; float yaw, scale; };
    SRWLOCK g_mu = SRWLOCK_INIT;
    std::vector<Req> g_queue;
    std::unordered_map<int, std::string> g_names;     // id → actor name, guarded by g_mu (written by the game thread)
    std::atomic<bool> g_pending{false}, g_listening{false};

    // catalog cache, guarded by g_mu (the walk itself is game thread, see CatalogNow)
    std::vector<props_spawn::Item> g_catalog;
    bool g_catalogAsked = false;
    std::vector<props_spawn::Item> CatalogNow();

    // game thread
    std::unordered_map<int, ref::Ref> g_actors;
    ref::Ref g_world;

    void Run(const Req& r) {
        UWorld* w = UWorld::GetWorld();
        if (!PtrOk(w)) return;
        if (!g_world.Is(w)) {  // a new map: earlier spawns are gone with the old one
            g_world = ref::Ref(w);
            g_actors.clear();
            AcquireSRWLockExclusive(&g_mu);
            g_names.clear();
            ReleaseSRWLockExclusive(&g_mu);
        }
        if (r.remove) {
            if (auto it = g_actors.find(r.id); it != g_actors.end()) {
                if (auto* a = it->second.Get<AActor>()) a->K2_DestroyActor();
                g_actors.erase(it);
            }
            AcquireSRWLockExclusive(&g_mu);
            g_names.erase(r.id);
            ReleaseSRWLockExclusive(&g_mu);
            return;
        }
        if (g_actors.count(r.id) && g_actors[r.id].Get<AActor>()) return;  // already there: moves go through props::Move
        const std::wstring wpath(r.mesh.begin(), r.mesh.end());
        UObject* o = UKismetSystemLibrary::LoadAsset_Blocking(
            UKismetSystemLibrary::Conv_SoftObjPathToSoftObjRef(UKismetSystemLibrary::MakeSoftObjectPath(FString(wpath.c_str()))));
        if (!PtrOk(o) || !o->IsA(UStaticMesh::StaticClass())) { logger::log("[props] no mesh " + r.mesh); return; }
        const FTransform t = UKismetMathLibrary::MakeTransform(r.at, FRotator{0.0f, r.yaw, 0.0f}, FVector{r.scale, r.scale, r.scale});
        AActor* a = UGameplayStatics::BeginDeferredActorSpawnFromClass(w, AStaticMeshActor::StaticClass(), t,
                                                                       ESpawnActorCollisionHandlingMethod::AlwaysSpawn, nullptr);
        if (!PtrOk(a)) return;
        UGameplayStatics::FinishSpawningActor(a, t);
        UStaticMeshComponent* c = static_cast<AStaticMeshActor*>(a)->StaticMeshComponent;
        if (PtrOk(c)) {
            c->SetMobility(EComponentMobility::Movable);
            c->SetStaticMesh(static_cast<UStaticMesh*>(o));
        }
        a->SetActorScale3D(FVector{r.scale, r.scale, r.scale});
        g_actors[r.id] = ref::Ref(a);
        AcquireSRWLockExclusive(&g_mu);
        g_names[r.id] = a->GetName();
        ReleaseSRWLockExclusive(&g_mu);
        logger::log("[props] spawned " + std::to_string(r.id) + " " + r.mesh + " as " + a->GetName());
    }

    void OnEvent(void*, void*, void*) {
        if (!game::OnGameThread() || !g_pending.exchange(false)) return;
        std::vector<Req> batch;
        bool catalog = false;
        AcquireSRWLockExclusive(&g_mu);
        batch.swap(g_queue);
        catalog = g_catalogAsked;
        g_catalogAsked = false;
        ReleaseSRWLockExclusive(&g_mu);
        for (const Req& r : batch) Run(r);
        if (!catalog) return;
        std::vector<props_spawn::Item> found = CatalogNow();
        AcquireSRWLockExclusive(&g_mu);
        g_catalog.swap(found);
        ReleaseSRWLockExclusive(&g_mu);
    }

    void Post(const Req& r) {
        AcquireSRWLockExclusive(&g_mu);
        g_queue.push_back(r);
        ReleaseSRWLockExclusive(&g_mu);
        g_pending = true;
        if (!g_listening.exchange(true)) game::OnGameTick(OnEvent, true);
    }

    void Wake() {  // ask the game thread for the catalog, nothing queued
        g_pending = true;
        if (!g_listening.exchange(true)) game::OnGameTick(OnEvent, true);
    }

    // Game thread. ponytail: walks the loaded levels' actors once per map, like props::NearNow.
    std::vector<props_spawn::Item> CatalogNow() {
        UWorld* w = UWorld::GetWorld();
        std::vector<props_spawn::Item> out;
        if (!PtrOk(w)) return out;
        const std::string wn = w->GetName();
        std::set<std::string> seen;
        UClass* smcCls = UStaticMeshComponent::StaticClass();
        for (int li = 0; li < w->Levels.Num(); li++) {  // once per map
            ULevel* lvl = w->Levels[li];
            if (!PtrOk(lvl)) continue;
            for (int ai = 0; ai < lvl->Actors.Num(); ai++) {
                AActor* a = lvl->Actors[ai];
                if (!PtrOk(a) || !PtrOk(a->RootComponent) || !a->RootComponent->IsA(smcCls)) continue;
                UStaticMesh* m = static_cast<UStaticMeshComponent*>(a->RootComponent)->StaticMesh;
                if (!PtrOk(m)) continue;
                const FVector s = a->RootComponent->RelativeScale3D;
                const float r = m->ExtendedBounds.SphereRadius * std::fmax(std::fabs(s.X), std::fabs(s.Z));
                if (r > 400.0f || r < 10.0f) continue;
                const std::string path = PathOf(m);
                if (seen.insert(path).second) out.push_back({path, m->GetName()});
            }
        }
        std::sort(out.begin(), out.end(), [](const props_spawn::Item& a, const props_spawn::Item& b) { return a.label < b.label; });
        logger::log("[props] catalog: " + std::to_string(out.size()) + " meshes in " + wn);
        return out;
    }
}

std::vector<props_spawn::Item> props_spawn::Catalog() {
    bool ask = false;
    {
        AcquireSRWLockExclusive(&g_mu);
        ask = !g_catalogAsked;
        if (ask) g_catalogAsked = true;
        ReleaseSRWLockExclusive(&g_mu);
    }
    if (ask) Wake();
    AcquireSRWLockShared(&g_mu);
    std::vector<props_spawn::Item> out = g_catalog;
    ReleaseSRWLockShared(&g_mu);
    return out;
}

void props_spawn::Spawn(int id, const std::string& mesh, float x, float y, float z, float yaw, float scale) {
    Post({id, false, mesh, FVector{x, y, z}, yaw, scale});
}

void props_spawn::Remove(int id) { Post({id, true, "", {}, 0, 1}); }

std::string props_spawn::NameOf(int id) {
    AcquireSRWLockShared(&g_mu);
    auto it = g_names.find(id);
    std::string n = it == g_names.end() ? "" : it->second;
    ReleaseSRWLockShared(&g_mu);
    return n;
}

int props_spawn::IdOf(const std::string& actorName) {
    AcquireSRWLockShared(&g_mu);
    int id = -1;
    for (const auto& [k, v] : g_names) if (v == actorName) { id = k; break; }
    ReleaseSRWLockShared(&g_mu);
    return id;
}

void props_spawn::Stop() {
    for (int i = 0; i < 50 && g_pending.load(); i++) Sleep(2);
    if (g_listening.exchange(false)) game::OnGameTick(OnEvent, false);
}


#include "mini_map.hpp"
#include "mini_map_math.hpp"
#include "game.hpp"
#include "logger.hpp"
#include "ref.hpp"

#include <Windows.h>
#include <atomic>
#include <cstdio>
#include "Engine_classes.hpp"

using namespace SDK;

namespace {
    bool PtrOk(const void* p) {
        const uintptr_t v = reinterpret_cast<uintptr_t>(p);
        return v > 0x10000 && v < 0x7FFFFFFFFFFFull;
    }
    bool Has(const std::string& s, const char* part) { return s.find(part) != std::string::npos; }

    // render thread → game thread
    SRWLOCK g_mu = SRWLOCK_INIT;
    mini_map::Desk g_desk{};
    std::atomic<int> g_req{0};  // 0 none, 1 place g_desk, 2 restore
    std::atomic<bool> g_listening{false};
    std::vector<mini_map::Target> g_targets;  // guarded by g_mu

    // game thread: the scene as the hub built it
    struct Orig { ref::Ref actor; mini_map::Xf xf; FRotator rot; bool button; std::string name; };
    std::vector<Orig> g_orig;
    ref::Ref g_world;
    float g_cx = 0, g_cy = 0, g_cz = 0;
    constexpr float kSceneWidth = 5000.0f;  // diorama extent (map probe, 2026-10-08)
    constexpr float kRadius = 3500.0f;      // scene actors around the buttons' centre

    bool Collect(UWorld* w) {
        g_orig.clear();
        std::vector<AActor*> buttons;
        for (int li = 0; li < w->Levels.Num(); li++) {
            ULevel* lvl = w->Levels[li];
            if (!PtrOk(lvl)) continue;
            for (int ai = 0; ai < lvl->Actors.Num(); ai++) {
                AActor* a = lvl->Actors[ai];
                if (PtrOk(a) && PtrOk(a->RootComponent) && a->GetName().rfind("Button_Map_", 0) == 0) buttons.push_back(a);
            }
        }
        if (buttons.empty()) return false;
        g_cx = g_cy = g_cz = 0;
        for (AActor* b : buttons) { const FVector p = b->RootComponent->RelativeLocation; g_cx += p.X; g_cy += p.Y; g_cz += p.Z; }
        g_cx /= buttons.size(); g_cy /= buttons.size(); g_cz /= buttons.size();
        for (int li = 0; li < w->Levels.Num(); li++) {
            ULevel* lvl = w->Levels[li];
            if (!PtrOk(lvl)) continue;
            for (int ai = 0; ai < lvl->Actors.Num(); ai++) {
                AActor* a = lvl->Actors[ai];
                if (!PtrOk(a) || !PtrOk(a->Class) || !PtrOk(a->RootComponent)) continue;
                const FVector p = a->RootComponent->RelativeLocation;
                const float dx = p.X - g_cx, dy = p.Y - g_cy, dz = p.Z - g_cz;
                if (dx * dx + dy * dy > kRadius * kRadius || std::fabs(dz) > 1500.0f) continue;
                const std::string cls = a->Class->GetName();
                const bool button = Has(cls, "TriggerVolumeButton");
                if (!button && (Has(cls, "Light") || Has(cls, "Volume") || Has(cls, "WorldMap_C"))) continue;  // stay put
                if (a->IsA(APawn::StaticClass())) continue;
                const FRotator r = a->RootComponent->RelativeRotation;
                g_orig.push_back({ref::Ref(a), {p.X, p.Y, p.Z, r.Yaw, a->RootComponent->RelativeScale3D.X}, r,
                                  Has(cls, "TriggerVolumeButton"), a->GetName()});
            }
        }
        char buf[160];
        std::snprintf(buf, sizeof(buf), "[mini-map] scene: %d actors, %d click zones, centre %.0f %.0f %.0f", int(g_orig.size()),
                      int(buttons.size()), g_cx, g_cy, g_cz);
        logger::log(buf);
        return true;
    }

    void Move(AActor* a, const FVector& loc, const FRotator& rot, float scale) {
        a->RootComponent->SetMobility(EComponentMobility::Movable);  // level actors are static: moves are refused otherwise
        a->K2_SetActorLocationAndRotation(loc, rot, false, nullptr, true);
        const FVector s0 = a->RootComponent->RelativeScale3D;
        const float f = s0.X != 0.0f ? scale / s0.X : 1.0f;  // keep non-uniform scales' proportions
        a->SetActorScale3D(FVector{s0.X * f, s0.Y * f, s0.Z * f});
    }

    void Apply(int req) {
        UWorld* w = UWorld::GetWorld();
        if (!PtrOk(w)) return;
        if (!g_world.Is(w)) { g_world = ref::Ref(w); g_orig.clear(); }
        std::vector<mini_map::Target> targets;
        if (req == 2) {
            for (const Orig& o : g_orig)
                if (auto* a = o.actor.Get<AActor>()) Move(a, FVector{o.xf.x, o.xf.y, o.xf.z}, o.rot, o.xf.scale);
            logger::log("[mini-map] restored");
        } else {
            if (g_orig.empty() && !Collect(w)) { logger::log("[mini-map] no world map scene in this map"); return; }
            AcquireSRWLockShared(&g_mu);
            const mini_map::Desk d = g_desk;
            ReleaseSRWLockShared(&g_mu);
            const float k = d.width / kSceneWidth;
            for (const Orig& o : g_orig) {
                auto* a = o.actor.Get<AActor>();
                if (!a) continue;
                const mini_map::Xf n = mini_map::Shrink(o.xf, g_cx, g_cy, g_cz, d.x, d.y, d.z, d.yaw, k);
                Move(a, FVector{n.x, n.y, n.z}, FRotator{o.rot.Pitch, n.yaw, o.rot.Roll}, n.scale);
                if (o.button) targets.push_back({o.name, n.x, n.y, n.z});
            }
            char buf[160];
            std::snprintf(buf, sizeof(buf), "[mini-map] placed at %.0f %.0f %.0f yaw %.0f, %.0f cm wide (1:%.0f)", d.x, d.y, d.z, d.yaw, d.width, 1.0f / k);
            logger::log(buf);
        }
        AcquireSRWLockExclusive(&g_mu);
        g_targets.swap(targets);
        ReleaseSRWLockExclusive(&g_mu);
    }

    void OnEvent(void*, void*, void*) {
        if (!game::OnGameThread()) return;
        if (const int r = g_req.exchange(0)) Apply(r);
    }

    void Listen() { if (!g_listening.exchange(true)) game::SetEventListener(OnEvent, true); }
}

void mini_map::Place(const Desk& d) {
    AcquireSRWLockExclusive(&g_mu);
    g_desk = d;
    ReleaseSRWLockExclusive(&g_mu);
    g_req = 1;
    Listen();
}

void mini_map::Restore() { g_req = 2; Listen(); }

std::vector<mini_map::Target> mini_map::Targets() {
    AcquireSRWLockShared(&g_mu);
    std::vector<Target> t = g_targets;
    ReleaseSRWLockShared(&g_mu);
    return t;
}

void mini_map::Stop() {
    for (int i = 0; i < 50 && g_req.load() != 0; i++) Sleep(2);
    if (g_listening.exchange(false)) game::SetEventListener(OnEvent, false);
}

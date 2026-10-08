#include "map_probe.hpp"
#include "game.hpp"
#include "logger.hpp"

#include <Windows.h>
#include <atomic>
#include <cstdio>
#include <string>
#include "Engine_classes.hpp"

using namespace SDK;

namespace {
    bool PtrOk(const void* p) {
        const uintptr_t v = reinterpret_cast<uintptr_t>(p);
        return v > 0x10000 && v < 0x7FFFFFFFFFFFull;
    }

    std::string GamePath(const char* file) {
        char buf[MAX_PATH] = {};
        GetModuleFileNameA(nullptr, buf, MAX_PATH);
        std::string p = buf;
        return p.substr(0, p.find_last_of("\/") + 1) + file;
    }

    void Line(const char* fmt, auto... a) {
        char buf[512];
        std::snprintf(buf, sizeof(buf), fmt, a...);
        logger::log(buf);
    }

    void Survey(UWorld* w) {
        AActor* map = nullptr;
        for (int li = 0; li < w->Levels.Num() && !map; li++) {
            ULevel* lvl = w->Levels[li];
            if (!PtrOk(lvl)) continue;
            for (int ai = 0; ai < lvl->Actors.Num(); ai++) {
                AActor* a = lvl->Actors[ai];
                if (PtrOk(a) && PtrOk(a->Class) && a->Class->GetName().rfind("BP_WorldMap", 0) == 0 && PtrOk(a->RootComponent)) { map = a; break; }
            }
        }
        if (!map) { logger::log("[map] no BP_WorldMap actor in this world"); return; }
        const FVector o = map->RootComponent->RelativeLocation;
        Line("[map] %s %s at %.0f %.0f %.0f scale %.2f mobility %d", map->Class->GetName().c_str(), map->GetName().c_str(), o.X, o.Y, o.Z,
             map->RootComponent->RelativeScale3D.X, int(map->RootComponent->Mobility));
        // its components: everything the scene might be made of
        for (int ci = 0; ci < map->BlueprintCreatedComponents.Num(); ci++) {
            UActorComponent* c = map->BlueprintCreatedComponents[ci];
            if (!PtrOk(c) || !PtrOk(c->Class)) continue;
            std::string extra;
            if (c->IsA(USceneComponent::StaticClass())) {
                auto* sc = static_cast<USceneComponent*>(c);
                char b[160];
                std::snprintf(b, sizeof(b), " rel %.0f %.0f %.0f scale %.2f", sc->RelativeLocation.X, sc->RelativeLocation.Y,
                              sc->RelativeLocation.Z, sc->RelativeScale3D.X);
                extra = b;
            }
            if (c->IsA(UStaticMeshComponent::StaticClass())) {
                UStaticMesh* m = static_cast<UStaticMeshComponent*>(c)->StaticMesh;
                if (PtrOk(m)) extra += " mesh " + m->GetName();
            }
            Line("[map]   component %s %s%s", c->Class->GetName().c_str(), c->GetName().c_str(), extra.c_str());
        }
        // neighbours: buttons, cameras, separate props of the scene
        int n = 0;
        for (int li = 0; li < w->Levels.Num(); li++) {
            ULevel* lvl = w->Levels[li];
            if (!PtrOk(lvl)) continue;
            for (int ai = 0; ai < lvl->Actors.Num(); ai++) {
                AActor* a = lvl->Actors[ai];
                if (!PtrOk(a) || a == map || !PtrOk(a->Class) || !PtrOk(a->RootComponent)) continue;
                const FVector p = a->RootComponent->RelativeLocation;
                const float dx = p.X - o.X, dy = p.Y - o.Y, dz = p.Z - o.Z;
                if (dx * dx + dy * dy + dz * dz > 8000.0f * 8000.0f) continue;
                n++;
                Line("[map]   near %s %s at %.0f %.0f %.0f (rel %.0f %.0f %.0f) scale %.2f mobility %d", a->Class->GetName().c_str(),
                     a->GetName().c_str(), p.X, p.Y, p.Z, dx, dy, dz, a->RootComponent->RelativeScale3D.X, int(a->RootComponent->Mobility));
            }
        }
        Line("[map] %d actors within 8000", n);
    }

    std::atomic<bool> g_asked{false}, g_listening{false};

    void OnEvent(void*, void*, void*) {  // the survey walks the levels' actors: game thread only (#80)
        if (!game::OnGameThread() || !g_asked.load()) return;
        UWorld* w = UWorld::GetWorld();
        if (PtrOk(w) && w->GetName() == "Hub") Survey(w);  // the map scene lives in the hub
        else logger::log("[map] not in the hub: no survey");
        g_asked = false;
    }
}

void map_probe::Tick() {
    static ULONGLONG next = 0;
    const ULONGLONG now = GetTickCount64();
    if (now < next) return;
    next = now + 2000;
    if (g_asked.load()) return;
    if (g_listening.exchange(false)) game::SetEventListener(OnEvent, false);  // the survey ran
    const std::string f = GamePath("dos-tool-mapsurvey.txt");
    if (GetFileAttributesA(f.c_str()) == INVALID_FILE_ATTRIBUTES) return;
    MoveFileExA(f.c_str(), (f + ".done").c_str(), MOVEFILE_REPLACE_EXISTING);
    g_asked = true;
    g_listening = true;
    game::SetEventListener(OnEvent, true);
}

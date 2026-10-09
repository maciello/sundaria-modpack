#include "mini_map.hpp"
#include "mini_map_math.hpp"
#include "game.hpp"
#include "logger.hpp"
#include "ref.hpp"

#include <Windows.h>
#include <atomic>
#include <cstdio>
#include <unordered_map>
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
    constexpr float kPlateauZ = 6000.0f;    // the map stands at z ≈ 7600; anywhere lower it has been moved

    std::string GamePath(const char* file) {
        char buf[MAX_PATH] = {};
        GetModuleFileNameA(nullptr, buf, MAX_PATH);
        std::string p = buf;
        return p.substr(0, p.find_last_of("\\/") + 1) + file;
    }

    std::string ReadText(const std::string& path) {
        std::string out;
        HANDLE h = CreateFileA(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
        if (h == INVALID_HANDLE_VALUE) return out;
        char buf[4096];
        DWORD n = 0;
        while (ReadFile(h, buf, sizeof(buf), &n, nullptr) && n > 0) out.append(buf, n);
        CloseHandle(h);
        return out;
    }

    void WriteText(const std::string& path, const std::string& text) {
        HANDLE h = CreateFileA(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h == INVALID_HANDLE_VALUE) return;
        DWORD n = 0;
        WriteFile(h, text.data(), DWORD(text.size()), &n, nullptr);
        CloseHandle(h);
    }

    template <class F> void ForEachActor(UWorld* w, F&& fn) {
        for (int li = 0; li < w->Levels.Num(); li++) {
            ULevel* lvl = w->Levels[li];
            if (!PtrOk(lvl)) continue;
            for (int ai = 0; ai < lvl->Actors.Num(); ai++) {
                AActor* a = lvl->Actors[ai];
                if (PtrOk(a) && PtrOk(a->Class) && PtrOk(a->RootComponent)) fn(a);
            }
        }
    }

    // First run only: the scene read off the plateau. Refuses when the map's click zones are not up there (already
    // moved by an earlier run): then every actor around them would be taken for the map - the tavern included.
    bool Record(UWorld* w, mini_map::Scene& sc) {
        std::vector<AActor*> buttons;
        ForEachActor(w, [&](AActor* a) { if (a->GetName().rfind("Button_Map_", 0) == 0) buttons.push_back(a); });
        if (buttons.empty()) return false;
        for (AActor* b : buttons) { const FVector p = b->RootComponent->RelativeLocation; sc.cx += p.X; sc.cy += p.Y; sc.cz += p.Z; }
        sc.cx /= buttons.size(); sc.cy /= buttons.size(); sc.cz /= buttons.size();
        if (sc.cz < kPlateauZ) { logger::log("[mini-map] the world map is not on its plateau and no scene was recorded: reload the hub"); return false; }
        ForEachActor(w, [&](AActor* a) {
            const FVector p = a->RootComponent->RelativeLocation;
            const float dx = p.X - sc.cx, dy = p.Y - sc.cy, dz = p.Z - sc.cz;
            if (dx * dx + dy * dy > kRadius * kRadius || std::fabs(dz) > 1500.0f || a->IsA(APawn::StaticClass())) return;
            const std::string cls = a->Class->GetName();
            const bool button = Has(cls, "TriggerVolumeButton");
            if (!button && (Has(cls, "Light") || Has(cls, "Volume") || Has(cls, "WorldMap_C"))) return;  // stay put
            const FRotator r = a->RootComponent->RelativeRotation;
            sc.actors.push_back({a->GetName(), p.X, p.Y, p.Z, r.Pitch, r.Yaw, r.Roll, a->RootComponent->RelativeScale3D.X, button});
        });
        return !sc.actors.empty();
    }

    bool Collect(UWorld* w) {
        g_orig.clear();
        mini_map::Scene sc;
        const std::string file = GamePath("dos-tool-worldmap.ini");
        if (!mini_map::ReadScene(ReadText(file), sc)) {
            if (!Record(w, sc)) return false;
            WriteText(file, mini_map::WriteScene(sc));
            logger::log("[mini-map] scene recorded to dos-tool-worldmap.ini");
        }
        g_cx = sc.cx; g_cy = sc.cy; g_cz = sc.cz;
        std::unordered_map<std::string, const mini_map::SceneActor*> want;
        for (const mini_map::SceneActor& sa : sc.actors) want[sa.name] = &sa;
        ForEachActor(w, [&](AActor* a) {
            auto it = want.find(a->GetName());
            if (it == want.end()) return;
            const mini_map::SceneActor& sa = *it->second;
            g_orig.push_back({ref::Ref(a), {sa.x, sa.y, sa.z, sa.yaw, sa.scale}, FRotator{sa.pitch, sa.yaw, sa.roll}, sa.button, sa.name});
        });
        char buf[160];
        std::snprintf(buf, sizeof(buf), "[mini-map] scene: %d of %d recorded actors found, centre %.0f %.0f %.0f", int(g_orig.size()),
                      int(sc.actors.size()), g_cx, g_cy, g_cz);
        logger::log(buf);
        return !g_orig.empty();
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

    void Listen() { if (!g_listening.exchange(true)) game::OnGameTick(OnEvent, true); }
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
    if (g_listening.exchange(false)) game::OnGameTick(OnEvent, false);
}

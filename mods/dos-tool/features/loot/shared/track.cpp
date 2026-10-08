#include "loot.hpp"
#include "game.hpp"
#include "logger.hpp"
#include "ref.hpp"
#include "umg.hpp"
#include "cost.hpp"

#include <Windows.h>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <map>
#include <string>
#include "Engine_classes.hpp"
#include "BP_TriggerBase_classes.hpp"
#include "BP_WorldLootBase_classes.hpp"
#include "BP_WorldSingleItemtLoot_classes.hpp"
#include "BP_RimShaderComponent_classes.hpp"
#include "BP_ItemContainerComponent_classes.hpp"
#include "BP_AffixItemFactory_classes.hpp"
#include "BP_ArchonClientFunctionLibrary_classes.hpp"
#include "BP_ArchonClientFunctionLibrary_parameters.hpp"

// Loot tracker: candidates come from the level's own actor list (once per world) and from each loot actor's
// ReceiveBeginPlay (drops, broken barrels); never from GObjects. Positions are cached (loot does not move), so far
// actors cost a float compare per frame; only near ones are read.
using namespace SDK;
using umg::PtrOk;

namespace {
    struct Entry { ref::Ref a; loot::Kind kind; float x, y, z; };
    SRWLOCK g_mu = SRWLOCK_INIT;  // not std::mutex (gotchas). Guards g_list, g_near, g_colors.
    std::vector<Entry> g_list;
    std::vector<loot::Actor> g_near;        // last world tick's read around the camera: plain data for the render thread
    std::atomic<float> g_maxDist{0.0f};     // the render thread's last Read radius
    std::array<style::Rgba, 8> g_colors{};
    bool g_haveColors = false;
    ref::Ref g_world;  // game thread only
    ref::Fn g_beginPlay{AActor::StaticClass, "Actor", "ReceiveBeginPlay"};
    ref::Fn g_gradeColor{UBP_ArchonClientFunctionLibrary_C::StaticClass, "BP_ArchonClientFunctionLibrary_C", "GetItemColorForGrade"};

    bool IsLoot(const UObject* o, loot::Kind& kind) {
        UClass* item = ABP_WorldSingleItemtLoot_C::StaticClass();  // Blueprint classes: null until a world loads them (#54)
        UClass* chest = ABP_WorldLootBase_C::StaticClass();
        if (PtrOk(item) && o->IsA(item)) { kind = loot::Kind::Item; return true; }
        if (PtrOk(chest) && o->IsA(chest)) { kind = loot::Kind::Chest; return true; }
        return false;
    }

    UBP_RimShaderComponent_C* Rim(const ABP_TriggerBase_C* t, loot::Kind kind) {
        UBP_RimShaderComponent_C* r = kind == loot::Kind::Item ? static_cast<const ABP_WorldSingleItemtLoot_C*>(t)->RimShaderComponent
                                                               : static_cast<const ABP_WorldLootBase_C*>(t)->RimShaderComponent;
        return PtrOk(r) ? r : nullptr;
    }

    // Newest UPrimitiveComponent::LastRenderTimeOnScreen (native, after BoundsScale; same read as core SampleHealth) over
    // the meshes the game's rim highlight draws: the loot's visible meshes (CachedPrimitiveComp is a trigger, never
    // rendered). NaN = no mesh known.
    float Seen(const ABP_TriggerBase_C* t, loot::Kind kind) {
        static_assert(offsetof(UPrimitiveComponent, BoundsScale) == 0x284, "re-check LastRenderTimeOnScreen offset");
        float seen = NAN;
        if (UBP_RimShaderComponent_C* rim = Rim(t, kind))
            for (int i = 0; i < rim->RenderComponents.Num() && i < 8; i++)
                if (const UPrimitiveComponent* p = rim->RenderComponents[i]; PtrOk(p)) {
                    const float s = *reinterpret_cast<const float*>(reinterpret_cast<const uint8*>(p) + 0x290);
                    seen = std::isnan(seen) ? s : std::max(seen, s);
                }
        return seen;
    }

    loot::State StateOf(const ABP_TriggerBase_C* t, loot::Kind kind, int& grade) {
        loot::State s{kind, bool(t->bHidden), true, uint8(t->mOpenCloseAnimState)};
        grade = -1;
        if (kind == loot::Kind::Item) {
            auto* i = static_cast<const ABP_WorldSingleItemtLoot_C*>(t);
            s.ready = i->LootIsReady;
            grade = int(i->ItemGradeModifier);
        } else {
            auto* c = static_cast<const ABP_WorldLootBase_C*>(t);
            UBP_ItemContainerComponent_C* box = PtrOk(c->ItemContainerComponent) ? c->ItemContainerComponent : nullptr;
            s.ready = PtrOk(c->ItemFactoryComponent) && c->ItemFactoryComponent->IsLootReady;
            for (int k = 0; box && k < box->Items.Num(); k++) grade = std::max(grade, int(box->Items[k].Itemgrade_29_AE6419044A6E394815070E8A0964ED01));
        }
        return s;
    }

    int Items(const ABP_TriggerBase_C* t, loot::Kind kind) {
        if (kind == loot::Kind::Item) return 1;
        auto* box = static_cast<const ABP_WorldLootBase_C*>(t)->ItemContainerComponent;
        return PtrOk(box) ? box->Items.Num() : -1;
    }

    void Add(AActor* a, loot::Kind kind) {
        USceneComponent* root = PtrOk(a->RootComponent) ? a->RootComponent : nullptr;
        if (!root) return;
        const FVector& p = root->RelativeLocation;  // unattached root: relative == world
        const ref::Ref r(a);
        AcquireSRWLockExclusive(&g_mu);
        std::erase_if(g_list, [](const Entry& e) { return !e.a.Get(); });  // streamed-out / destroyed loot: O(tracked) per spawn
        if (std::none_of(g_list.begin(), g_list.end(), [&](const Entry& e) { return e.a == r; })) g_list.push_back({r, kind, p.X, p.Y, p.Z});
        ReleaseSRWLockExclusive(&g_mu);
    }

    std::string ClassName(const UObject* o) { return PtrOk(o) && PtrOk(o->Class) ? o->Class->GetName() : "-"; }

    // One line per tracked actor: what vanilla shows on it (particles, rim) next to the state we read.
    void Survey(const char* why) {
        AcquireSRWLockShared(&g_mu);
        const std::vector<Entry> list = g_list;
        ReleaseSRWLockShared(&g_mu);
        std::map<std::string, int> perClass;
        for (const Entry& e : list) {
            auto* t = e.a.Get<ABP_TriggerBase_C>();
            if (!t) continue;
            const std::string cls = ClassName(t);
            if (perClass[cls]++ >= 4) continue;  // detail for the first 4 of each class
            int grade;
            const loot::State s = StateOf(t, e.kind, grade);
            UParticleSystemComponent* ps = e.kind == loot::Kind::Item ? static_cast<ABP_WorldSingleItemtLoot_C*>(t)->LootParticleSystem
                                                                      : static_cast<ABP_WorldLootBase_C*>(t)->LootParticleSystem;
            UBP_RimShaderComponent_C* rim = Rim(t, e.kind);
            char b[400];
            std::snprintf(b, sizeof b,
                          "[loot] %s @ %.0f %.0f %.0f hidden=%d ready=%d anim=%d lock=%d open=%d items=%d grade=%d attached=%d unlooted=%d"
                          " | ps=%s active=%d visible=%d | rim use=%d vis=%d stencil=%d meshes=%d seen=%.1f",
                          cls.c_str(), e.x, e.y, e.z, s.hidden, s.ready, s.anim, int(t->LockStatus), t->CanBeOpened, Items(t, e.kind), grade,
                          PtrOk(t->RootComponent) && PtrOk(t->RootComponent->AttachParent), loot::Unlooted(s),
                          PtrOk(ps) ? ClassName(ps->Template).c_str() : "-", PtrOk(ps) && ps->bIsActive, PtrOk(ps) && ps->bVisible,
                          PtrOk(rim) && rim->UseRimShader, PtrOk(rim) && rim->Visualize, PtrOk(rim) ? rim->Stencil : -1,
                          PtrOk(rim) ? rim->RenderComponents.Num() : -1, Seen(t, e.kind));
            std::string line = b;
            if (PtrOk(ps) && PtrOk(ps->Template)) line += " template=" + ps->Template->GetName();
            logger::log(line);
        }
        std::string sum = std::string("[loot] ") + why + ": " + std::to_string(list.size()) + " tracked:";
        for (const auto& [c, n] : perClass) sum += " " + c + "×" + std::to_string(n);
        logger::log(sum);
    }

    void Scan(UWorld* w) {  // the world's own actor lists: once per world
        static cost::Path path{"loot scan (level actor lists)"};
        cost::Scope cs(path);
        AcquireSRWLockExclusive(&g_mu);
        g_list.clear();
        g_haveColors = false;
        ReleaseSRWLockExclusive(&g_mu);
        for (int li = 0; li < w->Levels.Num(); li++) {
            ULevel* lvl = w->Levels[li];
            if (!PtrOk(lvl)) continue;
            for (int ai = 0; ai < lvl->Actors.Num(); ai++) {
                AActor* a = lvl->Actors[ai];
                loot::Kind kind;
                if (PtrOk(a) && IsLoot(a, kind)) Add(a, kind);
            }
        }
    }

    // Game thread (world tick): the tracked actors near the camera, read while the game cannot free them.
    void Sample(float cx, float cy, float cz, float maxDist) {
        const float max2 = maxDist * maxDist;
        std::vector<loot::Actor> found;
        AcquireSRWLockShared(&g_mu);
        for (const Entry& e : g_list) {
            const float dx = e.x - cx, dy = e.y - cy, dz = e.z - cz;
            if (dx * dx + dy * dy + dz * dz > max2) continue;
            auto* t = e.a.Get<ABP_TriggerBase_C>();
            if (!t) continue;
            int grade;
            const loot::State s = StateOf(t, e.kind, grade);
            found.push_back({reinterpret_cast<uintptr_t>(t), e.kind, e.x, e.y, e.z, loot::Unlooted(s), grade, Seen(t, e.kind)});
        }
        ReleaseSRWLockShared(&g_mu);
        AcquireSRWLockExclusive(&g_mu);
        g_near.swap(found);
        ReleaseSRWLockExclusive(&g_mu);
    }

    void ReadColors() {
        UFunction* fn = g_gradeColor.Get();
        APlayerController* pc = umg::LocalPC();
        if (!fn || !pc) return;
        std::array<style::Rgba, 8> c{};
        std::string line = "[loot] grade colours:";
        for (int g = 0; g < 8; g++) {
            Params::BP_ArchonClientFunctionLibrary_C_GetItemColorForGrade p{};
            p.ItemGrade = EItemGrade(g);
            p.__WorldContext = pc;
            UBP_ArchonClientFunctionLibrary_C::GetDefaultObj()->ProcessEvent(fn, &p);
            c[g] = {loot::ToSrgb(p.ItemColor.R), loot::ToSrgb(p.ItemColor.G), loot::ToSrgb(p.ItemColor.B)};
            char b[32];
            std::snprintf(b, sizeof b, " %d=%d,%d,%d", g, c[g].r, c[g].g, c[g].b);
            line += b;
        }
        logger::log(line);
        AcquireSRWLockExclusive(&g_mu);
        g_colors = c;
        g_haveColors = true;
        ReleaseSRWLockExclusive(&g_mu);
    }
}

namespace loot {
    void OnEvent(void* objp, void* fnp) {
        auto* obj = static_cast<UObject*>(objp);
        if (!game::OnGameThread() || !PtrOk(obj) || !PtrOk(fnp)) return;
        const auto* fn = static_cast<const UFunction*>(fnp);
        // Overrides of ReceiveBeginPlay are separate UFunctions with the same name: compare names, not pointers.
        if (const UFunction* bp = g_beginPlay.Get(); bp && fn->Name == bp->Name) {
            Kind kind;
            if (IsLoot(obj, kind)) {
                Add(static_cast<AActor*>(obj), kind);
                logger::log("[loot] BeginPlay " + ClassName(obj) + " (" + std::to_string(Tracked()) + " tracked)");
            }
            return;
        }
        if (!umg::IsWorldTick(fnp)) return;
        UWorld* w = UWorld::GetWorld();
        if (!PtrOk(w)) return;
        if (!g_world.Is(w)) {
            g_world = ref::Ref(w);
            Scan(w);
            Survey("world scan");
        }
        if (!g_haveColors) ReadColors();
        combat::View v{};
        if (game::GetView(v)) Sample(v.x, v.y, v.z, g_maxDist.load());
    }

    void Read(float cx, float cy, float cz, float maxDist, std::vector<Actor>& out) {
        g_maxDist = maxDist;
        const float max2 = maxDist * maxDist;
        AcquireSRWLockShared(&g_mu);
        for (const Actor& a : g_near) {
            const float dx = a.x - cx, dy = a.y - cy, dz = a.z - cz;
            if (dx * dx + dy * dy + dz * dz <= max2) out.push_back(a);
        }
        ReleaseSRWLockShared(&g_mu);
    }

    bool GradeColors(std::array<style::Rgba, 8>& out) {
        AcquireSRWLockShared(&g_mu);
        const bool ok = g_haveColors;
        if (ok) out = g_colors;
        ReleaseSRWLockShared(&g_mu);
        return ok;
    }

    int Tracked() {
        AcquireSRWLockShared(&g_mu);
        const int n = int(g_list.size());
        ReleaseSRWLockShared(&g_mu);
        return n;
    }

    void Reset() {
        AcquireSRWLockExclusive(&g_mu);
        g_list.clear();
        g_near.clear();
        g_haveColors = false;
        ReleaseSRWLockExclusive(&g_mu);
        g_world = {};  // ponytail: written from the render thread while the listener is already off
    }
}

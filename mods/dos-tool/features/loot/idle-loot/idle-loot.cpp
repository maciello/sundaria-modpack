#include "feature.hpp"
#include "game.hpp"
#include "logger.hpp"
#include "ref.hpp"
#include "umg.hpp"
#include "drain.hpp"
#include "idle-loot.hpp"
#include "../shared/loot.hpp"

#include <Windows.h>
#include <atomic>
#include <cstdio>
#include <string>
#include <unordered_map>
#include <vector>
#include "Engine_classes.hpp"
#include "Engine_parameters.hpp"

// Idle loot sparkle (#27, #110): WoW lootable-corpse style. Each unlooted loot actor carries a game particle system
// (kTemplate) attached to its root, tinted with the best item grade around it through a material instance. In the
// world: depth-tested, under the game UI, moves with the actor. Game thread only, on loot events (BeginPlay,
// trigger/loot-ready/pickup/end-play): no per-frame work. Spec: references/design-system.md § Loot marker (idle sparkle).
using namespace SDK;
using namespace idle_loot;
using umg::CallNative;
using umg::PtrOk;

namespace {
    struct Fns {
        ref::Fn load{UKismetSystemLibrary::StaticClass, "KismetSystemLibrary", "LoadAsset_Blocking"};
        ref::Fn toName{UKismetStringLibrary::StaticClass, "KismetStringLibrary", "Conv_StringToName"};
        ref::Fn spawn{UGameplayStatics::StaticClass, "GameplayStatics", "SpawnEmitterAttached"};
        ref::Fn mid{UPrimitiveComponent::StaticClass, "PrimitiveComponent", "CreateDynamicMaterialInstance"};
        ref::Fn setMat{UPrimitiveComponent::StaticClass, "PrimitiveComponent", "SetMaterial"};
        ref::Fn cull{UPrimitiveComponent::StaticClass, "PrimitiveComponent", "SetCullDistance"};
        ref::Fn color{UMaterialInstanceDynamic::StaticClass, "MaterialInstanceDynamic", "SetVectorParameterValue"};
        ref::Fn destroy{UActorComponent::StaticClass, "ActorComponent", "K2_DestroyComponent"};
        bool Ok() {
            for (ref::Fn* f : {&load, &toName, &spawn, &mid, &setMat, &cull, &color, &destroy})
                if (!f->Get()) return false;
            return true;
        }
    } g_fn;

    FName Name(const wchar_t* s) {
        Params::KismetStringLibrary_Conv_StringToName p{};
        p.inString = FString(s);
        CallNative(UKismetStringLibrary::GetDefaultObj(), g_fn.toName.Get(), &p);
        return p.ReturnValue;
    }

    UParticleSystem* LoadTemplate() {
        UObject* o = UKismetSystemLibrary::LoadAsset_Blocking(
            UKismetSystemLibrary::Conv_SoftObjPathToSoftObjRef(UKismetSystemLibrary::MakeSoftObjectPath(FString(kTemplate))));
        const bool ok = PtrOk(o) && o->IsA(UParticleSystem::StaticClass());
        logger::log(std::string("[idle-loot] template ") + (ok ? "loaded" : "missing"));
        return ok ? static_cast<UParticleSystem*>(o) : nullptr;
    }
    ref::Cached<UParticleSystem> g_template{LoadTemplate};

    struct Spark {
        ref::Ref actor, psc, mid;
        int grade;
    };
    std::unordered_map<std::uintptr_t, Spark> g_sparks;  // key = loot actor address; game thread only
    std::atomic<bool> g_on{false};
    game::Drain g_drain;
    thread_local bool t_busy = false;
    FName g_glowName{};
    // Events after which a loot actor's state may have changed (each reaches ProcessEvent: native event, bound delegate
    // or received RPC/RepNotify); on any class (Blueprint overrides declare their own), filtered to loot in OnLootEvent.
    constexpr const char* kWatch[] = {"OnTriggerChanged", "OnLootReady", "OnRep_LootIsReady", "MulticastPlayPickupSound",
                                      "I_SetInactiveLoot", "ReceiveEndPlay"};
    bool g_named = false;  // game thread
    std::uint32_t g_version = ~0u;
    double g_settleUntil = 0;

    double Now() {
        LARGE_INTEGER t, f;
        QueryPerformanceCounter(&t);
        QueryPerformanceFrequency(&f);
        return double(t.QuadPart) / double(f.QuadPart);
    }

    void Remove(Spark& s) {  // the component dies with its actor; a live one is destroyed by name of its owner
        auto* a = s.actor.Get<AActor>();
        if (auto* c = s.psc.Get<UActorComponent>(); c && a) {
            Params::ActorComponent_K2_DestroyComponent p{a};  // UE refuses unless Object == the component's owner
            CallNative(c, g_fn.destroy.Get(), &p);
        }
    }
    void RemoveAll() {  // game thread (or Off() after its wait ran out)
        for (auto& [k, s] : g_sparks) Remove(s);
        g_sparks.clear();
    }

    void Tint(Spark& s, int grade, const std::array<style::Rgba, 8>& tiers) {
        auto* mid = s.mid.Get<UMaterialInstanceDynamic>();
        if (!mid) return;
        const Rgb c = Glow(grade, tiers);
        Params::MaterialInstanceDynamic_SetVectorParameterValue p{};
        p.ParameterName = g_glowName;
        p.Value = {c.r, c.g, c.b, 1.0f};
        CallNative(mid, g_fn.color.Get(), &p);
        s.grade = grade;
    }

    bool Attach(AActor* a, const loot::Actor& l, int grade, const std::array<style::Rgba, 8>& tiers, Spark& out) {
        UParticleSystem* t = g_template.Get();
        if (!t || !PtrOk(a->RootComponent)) return false;
        const bool chest = l.kind == loot::Kind::Chest;
        const float k = chest ? kScaleChest : kScaleItem;
        Params::GameplayStatics_SpawnEmitterAttached p{};
        p.EmitterTemplate = t;
        p.AttachToComponent = a->RootComponent;
        p.Location = {0, 0, chest ? kLiftChest : kLiftItem};
        p.Scale = {k, k, k};
        p.LocationType = EAttachLocation::KeepRelativeOffset;
        p.bAutoDestroy = false;
        p.PoolingMethod = EPSCPoolMethod::None;
        p.bAutoActivate = true;
        CallNative(UGameplayStatics::GetDefaultObj(), g_fn.spawn.Get(), &p);
        UParticleSystemComponent* psc = p.ReturnValue;
        if (!PtrOk(psc)) return false;
        Params::PrimitiveComponent_SetCullDistance cd{kCull};
        CallNative(psc, g_fn.cull.Get(), &cd);
        Params::PrimitiveComponent_CreateDynamicMaterialInstance m{};
        m.ElementIndex = 0;  // the template's one emitter; null source = its own material
        CallNative(psc, g_fn.mid.Get(), &m);
        if (PtrOk(m.ReturnValue)) {
            Params::PrimitiveComponent_SetMaterial sm{0, {}, m.ReturnValue};
            CallNative(psc, g_fn.setMat.Get(), &sm);
        }
        out = {ref::Ref(a), ref::Ref(psc), ref::Ref(PtrOk(m.ReturnValue) ? m.ReturnValue : nullptr), -3};
        Tint(out, grade, tiers);
        return true;
    }

    // Spawn on unlooted loot, recolour on grade change, remove from looted or gone loot. O(tracked loot).
    void Reconcile() {
        std::vector<loot::Actor> all;
        loot::Each(all);
        const std::vector<int> grades = PileGrades(all);
        std::array<style::Rgba, 8> tiers;
        if (!loot::GradeColors(tiers))
            for (int g = 0; g < 8; g++) tiers[g] = style::rarity::Of(g);
        std::unordered_map<std::uintptr_t, Spark> keep;
        for (size_t i = 0; i < all.size(); i++) {
            if (grades[i] == kLooted) continue;
            auto* a = reinterpret_cast<AActor*>(all[i].id);  // live: Each read it on this game-thread call
            auto it = g_sparks.find(all[i].id);
            if (it != g_sparks.end() && it->second.actor.Is(a) && it->second.psc.Get()) {
                if (it->second.grade != grades[i]) Tint(it->second, grades[i], tiers);
                keep.emplace(it->first, it->second);
                g_sparks.erase(it);
                continue;
            }
            Spark s;
            if (Attach(a, all[i], grades[i], tiers, s)) {
                keep.emplace(all[i].id, s);
                char b[120];
                std::snprintf(b, sizeof b, "[idle-loot] sparkle on %s %llx, grade %d", all[i].kind == loot::Kind::Chest ? "chest" : "item",
                              (unsigned long long)all[i].id, grades[i]);
                logger::log(b);
            }
        }
        for (auto& [k, s] : g_sparks) {  // looted, streamed out or destroyed
            Remove(s);
            logger::log("[idle-loot] sparkle off " + std::to_string(k));
        }
        g_sparks.swap(keep);
    }

    void OnLootEvent(void* obj, void*, void*) {  // a watched function ran: re-read loot state for kSettle
        if (game::OnGameThread() && !t_busy && loot::IsLootActor(obj)) g_settleUntil = Now() + kSettle;
    }

    void OnBeginPlay(void* obj, void* fn, void*) {
        if (!t_busy) loot::OnEvent(obj, fn);  // the tracker adds loot actors (Version() bumps)
    }

    void OnTick(void* obj, void* fn, void*) {
        if (t_busy || !game::OnGameThread()) return;
        loot::OnEvent(obj, fn);  // tracker: world scan on a new world, grade colours
        t_busy = true;
        if (g_drain.Serve(RemoveAll)) { t_busy = false; return; }
        if (!g_named && g_fn.Ok()) {
            g_glowName = Name(kGlowParam);
            g_named = true;
        }
        if (g_named && (loot::Version() != g_version || Now() < g_settleUntil)) {
            g_version = loot::Version();
            Reconcile();
        }
        t_busy = false;
    }

    void Listen(bool on) {
        game::On(nullptr, "ReceiveBeginPlay", &OnBeginPlay, on);
        for (const char* fn : kWatch) game::On(nullptr, fn, &OnLootEvent, on);
        game::OnWorldTick(&OnTick, on);
    }

    struct IdleLoot : feature::Feature {
        IdleLoot() : Feature("Idle loot sparkle", feature::Stage::Alpha) { optIn = true; }

        void OnFrame(const feature::Frame&) override {
            if (g_on.load()) return;
            g_version = ~0u;
            g_on = true;
            Listen(true);
        }

        // Off() runs with the ProcessEvent hook alive (#84): the next world tick destroys the components.
        void Off() override {
            g_drain.Request(!g_sparks.empty(), "idle-loot", RemoveAll);
            g_on = false;
            Listen(false);
            g_sparks.clear();
            loot::Reset();
        }
    } g_idle_loot;
}


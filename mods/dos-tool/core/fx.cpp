#include "fx.hpp"
#include "drain.hpp"
#include "game.hpp"
#include "logger.hpp"
#include "ref.hpp"
#include "umg.hpp"

#include <Windows.h>
#include <map>
#include <string>
#include <unordered_map>
#include <vector>
#include "Engine_classes.hpp"
#include "Engine_parameters.hpp"
#include "Niagara_classes.hpp"
#include "Niagara_parameters.hpp"

// Spawned components are owned by the actor they attach to (or the world for At): they die with it; ours are
// destroyed by Remove / Release. Registry and every UFunction call: game thread. (references/fx.md)
using namespace SDK;
using umg::CallNative;
using umg::PtrOk;

namespace {
    struct Fns {
        ref::Fn toName{UKismetStringLibrary::StaticClass, "KismetStringLibrary", "Conv_StringToName"};
        ref::Fn emitAttached{UGameplayStatics::StaticClass, "GameplayStatics", "SpawnEmitterAttached"};
        ref::Fn emitAt{UGameplayStatics::StaticClass, "GameplayStatics", "SpawnEmitterAtLocation"};
        ref::Fn niaAttached{UNiagaraFunctionLibrary::StaticClass, "NiagaraFunctionLibrary", "SpawnSystemAttached"};
        ref::Fn niaAt{UNiagaraFunctionLibrary::StaticClass, "NiagaraFunctionLibrary", "SpawnSystemAtLocation"};
        ref::Fn scale{USceneComponent::StaticClass, "SceneComponent", "SetRelativeScale3D"};
        ref::Fn cull{UPrimitiveComponent::StaticClass, "PrimitiveComponent", "SetCullDistance"};
        ref::Fn mid{UPrimitiveComponent::StaticClass, "PrimitiveComponent", "CreateDynamicMaterialInstance"};
        ref::Fn setMat{UPrimitiveComponent::StaticClass, "PrimitiveComponent", "SetMaterial"};
        ref::Fn matColor{UMaterialInstanceDynamic::StaticClass, "MaterialInstanceDynamic", "SetVectorParameterValue"};
        ref::Fn matFloat{UMaterialInstanceDynamic::StaticClass, "MaterialInstanceDynamic", "SetScalarParameterValue"};
        ref::Fn color{UFXSystemComponent::StaticClass, "FXSystemComponent", "SetColorParameter"};
        ref::Fn flt{UFXSystemComponent::StaticClass, "FXSystemComponent", "SetFloatParameter"};
        ref::Fn vec{UFXSystemComponent::StaticClass, "FXSystemComponent", "SetVectorParameter"};
        ref::Fn owner{UActorComponent::StaticClass, "ActorComponent", "GetOwner"};
        ref::Fn destroy{UActorComponent::StaticClass, "ActorComponent", "K2_DestroyComponent"};
    } g_fn;

    struct Entry {
        std::string owner;
        ref::Ref comp;
        std::vector<std::pair<int, ref::Ref>> mids;  // emitter element -> its dynamic material instance
    };
    std::unordered_map<fx::Id, Entry> g_live;  // game thread (Release's fallback: after its wait ran out)
    std::map<std::wstring, ref::Ref> g_templates;
    std::map<std::wstring, FName> g_names;
    fx::Id g_next = 1;
    bool g_ticking = false;
    std::string g_releasing;
    game::Drain g_drain;

    FName Name(const wchar_t* s) {
        auto it = g_names.find(s);
        if (it != g_names.end()) return it->second;
        Params::KismetStringLibrary_Conv_StringToName p{};
        p.inString = FString(s);
        CallNative(UKismetStringLibrary::GetDefaultObj(), g_fn.toName.Get(), &p);
        return g_names[s] = p.ReturnValue;
    }

    UObject* Template(const wchar_t* path) {
        if (UObject* o = g_templates[path].Get()) return o;
        UObject* o = UKismetSystemLibrary::LoadAsset_Blocking(
            UKismetSystemLibrary::Conv_SoftObjPathToSoftObjRef(UKismetSystemLibrary::MakeSoftObjectPath(FString(path))));
        const bool ok = PtrOk(o) && (o->IsA(UParticleSystem::StaticClass()) || o->IsA(UNiagaraSystem::StaticClass()));
        const std::wstring w(path);
        logger::log("[fx] template " + std::string(w.begin(), w.end()) + (ok ? " loaded" : " missing or not a particle system"));
        g_templates[path] = ref::Ref(ok ? o : nullptr);
        return ok ? o : nullptr;
    }

    void Destroy(Entry& e) {
        auto* c = e.comp.Get<UActorComponent>();
        if (!c) return;
        Params::ActorComponent_GetOwner o{};
        CallNative(c, g_fn.owner.Get(), &o);
        Params::ActorComponent_K2_DestroyComponent d{o.ReturnValue};  // UE destroys only when Object == the owner (gotchas)
        CallNative(c, g_fn.destroy.Get(), &d);
    }

    void ReleaseNow() {
        std::erase_if(g_live, [](auto& kv) {
            if (kv.second.owner != g_releasing) return false;
            Destroy(kv.second);
            return true;
        });
    }

    void Tick(void*, void*, void*) {  // world tick, subscribed while effects exist
        if (!game::OnGameThread()) return;
        if (g_drain.Serve(ReleaseNow)) return;
        std::erase_if(g_live, [](auto& kv) { return !kv.second.comp.Get(); });  // died with their actor / world
        if (g_live.empty()) game::OnWorldTick(&Tick, g_ticking = false);
    }

    UFXSystemComponent* Comp(fx::Id id) {
        auto it = g_live.find(id);
        return it == g_live.end() ? nullptr : it->second.comp.Get<UFXSystemComponent>();
    }

    fx::Id Spawn(const char* owner, const wchar_t* path, void* parent, const fx::Place& p) {
        UObject* t = Template(path);
        if (!t) return 0;
        const bool niagara = t->IsA(UNiagaraSystem::StaticClass());
        USceneComponent* attach = nullptr;
        if (parent) {
            auto* o = static_cast<UObject*>(parent);
            if (o->IsA(AActor::StaticClass())) attach = static_cast<AActor*>(o)->RootComponent;
            else if (o->IsA(USceneComponent::StaticClass())) attach = static_cast<USceneComponent*>(o);
            if (!PtrOk(attach)) return 0;
        }
        UFXSystemComponent* c = nullptr;
        const FVector at{p.x, p.y, p.z}, one{p.scale, p.scale, p.scale};
        if (niagara && attach) {
            Params::NiagaraFunctionLibrary_SpawnSystemAttached s{};
            s.SystemTemplate = static_cast<UNiagaraSystem*>(t), s.AttachToComponent = attach, s.Location = at;
            s.LocationType = EAttachLocation::KeepRelativeOffset, s.bAutoActivate = true, s.PoolingMethod = ENCPoolMethod::None;
            CallNative(UNiagaraFunctionLibrary::GetDefaultObj(), g_fn.niaAttached.Get(), &s);
            c = s.ReturnValue;
        } else if (niagara) {
            Params::NiagaraFunctionLibrary_SpawnSystemAtLocation s{};
            s.WorldContextObject = UWorld::GetWorld(), s.SystemTemplate = static_cast<UNiagaraSystem*>(t), s.Location = at, s.Scale = one;
            s.bAutoActivate = true, s.PoolingMethod = ENCPoolMethod::None;
            CallNative(UNiagaraFunctionLibrary::GetDefaultObj(), g_fn.niaAt.Get(), &s);
            c = s.ReturnValue;
        } else if (attach) {
            Params::GameplayStatics_SpawnEmitterAttached s{};
            s.EmitterTemplate = static_cast<UParticleSystem*>(t), s.AttachToComponent = attach, s.Location = at, s.Scale = one;
            s.LocationType = EAttachLocation::KeepRelativeOffset, s.PoolingMethod = EPSCPoolMethod::None, s.bAutoActivate = true;
            CallNative(UGameplayStatics::GetDefaultObj(), g_fn.emitAttached.Get(), &s);
            c = s.ReturnValue;
        } else {
            Params::GameplayStatics_SpawnEmitterAtLocation s{};
            s.WorldContextObject = UWorld::GetWorld(), s.EmitterTemplate = static_cast<UParticleSystem*>(t), s.Location = at, s.Scale = one;
            s.PoolingMethod = EPSCPoolMethod::None, s.bAutoActivateSystem = true;
            CallNative(UGameplayStatics::GetDefaultObj(), g_fn.emitAt.Get(), &s);
            c = s.ReturnValue;
        }
        if (!PtrOk(c)) return 0;
        if (niagara && attach) {  // SpawnSystemAttached takes no scale
            Params::SceneComponent_SetRelativeScale3D s{one};
            CallNative(c, g_fn.scale.Get(), &s);
        }
        if (p.cull > 0) {
            Params::PrimitiveComponent_SetCullDistance cd{p.cull};
            CallNative(c, g_fn.cull.Get(), &cd);
        }
        const fx::Id id = g_next++;
        g_live[id] = {owner ? owner : "", ref::Ref(c), {}};
        if (!g_ticking) game::OnWorldTick(&Tick, g_ticking = true);
        return id;
    }

    UMaterialInstanceDynamic* Mid(fx::Id id, int element) {
        auto it = g_live.find(id);
        auto* c = it == g_live.end() ? nullptr : it->second.comp.Get<UPrimitiveComponent>();
        if (!c) return nullptr;
        for (auto& [el, r] : it->second.mids)
            if (el == element)
                if (auto* m = r.Get<UMaterialInstanceDynamic>()) return m;
        Params::PrimitiveComponent_CreateDynamicMaterialInstance m{};
        m.ElementIndex = element;  // null source = the emitter's own material
        CallNative(c, g_fn.mid.Get(), &m);
        if (!PtrOk(m.ReturnValue)) return nullptr;
        Params::PrimitiveComponent_SetMaterial sm{element, {}, m.ReturnValue};
        CallNative(c, g_fn.setMat.Get(), &sm);
        it->second.mids.emplace_back(element, ref::Ref(m.ReturnValue));
        return m.ReturnValue;
    }
}

namespace fx {
    Id Attach(const char* owner, const wchar_t* path, void* parent, const Place& p) {
        return game::OnGameThread() && PtrOk(parent) ? Spawn(owner, path, parent, p) : 0;
    }
    Id At(const char* owner, const wchar_t* path, const Place& p) { return game::OnGameThread() ? Spawn(owner, path, nullptr, p) : 0; }
    bool Alive(Id id) { return Comp(id) != nullptr; }

    void Color(Id id, const wchar_t* name, float r, float g, float b, float a) {
        if (auto* c = Comp(id)) {
            Params::FXSystemComponent_SetColorParameter p{Name(name), {r, g, b, a}};
            CallNative(c, g_fn.color.Get(), &p);
        }
    }
    void Float(Id id, const wchar_t* name, float v) {
        if (auto* c = Comp(id)) {
            Params::FXSystemComponent_SetFloatParameter p{Name(name), v};
            CallNative(c, g_fn.flt.Get(), &p);
        }
    }
    void Vector(Id id, const wchar_t* name, float x, float y, float z) {
        if (auto* c = Comp(id)) {
            Params::FXSystemComponent_SetVectorParameter p{Name(name), {x, y, z}};
            CallNative(c, g_fn.vec.Get(), &p);
        }
    }
    void MaterialColor(Id id, int element, const wchar_t* name, float r, float g, float b, float a) {
        if (auto* m = Mid(id, element)) {
            Params::MaterialInstanceDynamic_SetVectorParameterValue p{Name(name), {r, g, b, a}};
            CallNative(m, g_fn.matColor.Get(), &p);
        }
    }
    void MaterialFloat(Id id, int element, const wchar_t* name, float v) {
        if (auto* m = Mid(id, element)) {
            Params::MaterialInstanceDynamic_SetScalarParameterValue p{Name(name), v};
            CallNative(m, g_fn.matFloat.Get(), &p);
        }
    }

    void Remove(Id id) {
        auto it = g_live.find(id);
        if (it == g_live.end()) return;
        Destroy(it->second);
        g_live.erase(it);
    }

    void Release(const char* owner) {
        g_releasing = owner ? owner : "";
        bool any = false;
        for (const auto& [id, e] : g_live) any |= e.owner == g_releasing;  // ponytail: racy read of the game thread's map, Off() only
        g_drain.Request(any && g_ticking, "fx", ReleaseNow);
    }
}

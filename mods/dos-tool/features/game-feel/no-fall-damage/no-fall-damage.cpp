#include "feature.hpp"
#include "game.hpp"
#include "logger.hpp"
#include "ref.hpp"
#include "drain.hpp"
#include "umg.hpp"  // umg::PtrOk
#include "imgui.h"

#include <atomic>
#include <string>
#include <vector>
#include "Engine_classes.hpp"
#include "Archon_classes.hpp"
#include "GameplayAbilities_classes.hpp"
#include "GameplayAbilities_parameters.hpp"
#include "BP_CharacterBase_classes.hpp"

// No fall damage (#143): the game's own BP_GameplayEffect_ImmuneFallDamage (Infinite, grants owned tag
// AbilityEffect.Damage.FallDamage) on every player character. Facts (skill game-facts.md fall_damage): ACharacter::Landed
// -> OnLanded (ProcessEvent) -> BP_CharacterBase::HandleOnLanded (server only) -> ApplyFallingDamage, which returns before
// ApplyDamage while the ASC has that tag (and no .Capped subtag). A pre-call filter on OnLanded puts the effect on the
// landing player's ASC first (authority only: the host decides in co-op), so it is there before the first damage check
// and on every respawned or new character without a timer. Off: the effects we added are removed on the world tick.
namespace {
    using namespace SDK;
    using umg::PtrOk;

    constexpr const wchar_t* kEffectPath =
        L"/Game/Blueprints/Ability/GameplayEffects/BP_GameplayEffect_ImmuneFallDamage.BP_GameplayEffect_ImmuneFallDamage_C";

    std::atomic<bool> g_on{false};
    std::atomic<int32_t> g_nLanded{-1};  // FName index of OnLanded, resolved on the world tick
    std::atomic<int> g_applied{0};       // effects currently on (menu)
    ref::Ref g_effect;                   // the GE class, loaded on the world tick
    ref::Fn g_fnLanded{ACharacter::StaticClass, "Character", "OnLanded"};
    ref::Fn g_fnMakeContext{UAbilitySystemComponent::StaticClass, "AbilitySystemComponent", "MakeEffectContext"};
    ref::Fn g_fnApplySelf{UAbilitySystemComponent::StaticClass, "AbilitySystemComponent", "BP_ApplyGameplayEffectToSelf"};
    game::Drain g_drain;

    struct Applied { ref::Ref asc; int handle; };
    std::vector<Applied> g_list;  // game thread only

    void Call(const UObject* obj, UFunction* fn, void* parms) {
        const auto flags = fn->FunctionFlags;
        fn->FunctionFlags |= 0x400;  // FUNC_Native
        obj->ProcessEvent(fn, parms);
        fn->FunctionFlags = flags;
    }

    // Game thread: our effect on this player's ASC, once per ASC (a new character brings a new ASC).
    void Ensure(ABP_CharacterBase_C* c) {
        UAbilitySystemComponent* asc = c->mAbilitySystemComponent;
        auto* cls = g_effect.Get<UClass>();
        UFunction* make = g_fnMakeContext.Get();
        UFunction* apply = g_fnApplySelf.Get();
        if (!PtrOk(asc) || !cls || !make || !apply) return;
        std::erase_if(g_list, [](const Applied& a) { return !a.asc.Get(); });  // characters gone with their ASC
        for (const Applied& a : g_list) if (a.asc.Is(asc)) return;
        Params::AbilitySystemComponent_MakeEffectContext ctx{};
        Call(asc, make, &ctx);
        Params::AbilitySystemComponent_BP_ApplyGameplayEffectToSelf p{};
        p.GameplayEffectClass = cls;
        p.Level = 1.0f;
        p.EffectContext = ctx.ReturnValue;
        Call(asc, apply, &p);
        const int handle = *reinterpret_cast<const int32*>(&p.ReturnValue);
        g_list.push_back({ref::Ref(asc), handle});
        g_applied = int(g_list.size());
        logger::log("[no-fall-damage] immune: " + c->GetName() + " (handle " + std::to_string(handle) + ")");
    }

    // Before the game's call (any thread: O(1) rejects first). A landing player character on the host gets the effect.
    bool Filter(void* obj, void* fn, void*) {
        const int32_t n = g_nLanded.load(std::memory_order_relaxed);
        if (n < 0 || !fn || static_cast<UFunction*>(fn)->Name.ComparisonIndex != n) return false;
        auto* o = static_cast<UObject*>(obj);
        if (!g_on || !game::OnGameThread() || !PtrOk(o) || !o->IsA(ABP_CharacterBase_C::StaticClass())) return false;
        auto* c = static_cast<ABP_CharacterBase_C*>(o);
        if (PtrOk(c->PlayerState) && c->Role == ENetRole::ROLE_Authority) Ensure(c);
        return false;
    }

    // World tick after enabling: load the effect class and resolve OnLanded's name, then stop listening.
    void Prepare(void*, void*, void*) {
        if (!g_on || !game::OnGameThread()) return;
        static bool tried = false;  // a missing class stays missing: one blocking load per session
        if (!g_effect.Get() && !tried) {
            tried = true;
            UObject* o = UKismetSystemLibrary::LoadAsset_Blocking(
                UKismetSystemLibrary::Conv_SoftObjPathToSoftObjRef(UKismetSystemLibrary::MakeSoftObjectPath(FString(kEffectPath))));
            const bool ok = PtrOk(o) && o->IsA(UClass::StaticClass()) && static_cast<UClass*>(o)->IsSubclassOf(UGameplayEffect::StaticClass());
            logger::log(std::string("[no-fall-damage] effect class ") + (ok ? "loaded" : "missing"));
            g_effect = ref::Ref(ok ? o : nullptr);
        }
        if (UFunction* f = g_fnLanded.Get(); PtrOk(f)) g_nLanded = f->Name.ComparisonIndex;
        if ((g_effect.Get() || tried) && g_nLanded >= 0) game::OnWorldTick(&Prepare, false);
    }

    void RemoveAll() {
        for (const Applied& a : g_list)
            if (auto* asc = a.asc.Get()) game::RemoveEffect(asc, a.handle);
        if (!g_list.empty()) logger::log("[no-fall-damage] removed " + std::to_string(g_list.size()) + " effect(s): vanilla fall damage");
        g_list.clear();
        g_applied = 0;
    }

    struct NoFallDamage : feature::Feature {
        NoFallDamage() : Feature("No fall damage", feature::Stage::Alpha) {}

        void OnFrame(const feature::Frame&) override {
            if (g_on.exchange(true)) return;
            g_drain.Cancel();  // re-enabled before the removal ran: keep what is on
            game::OnWorldTick(&Prepare, true);
            game::SetEventFilter(&Filter, true);
        }

        // Render thread: unsubscribe, hand the removal to the game thread.
        void Off() override {
            g_on = false;
            game::SetEventFilter(&Filter, false);
            game::OnWorldTick(&Prepare, false);
            g_drain.Request(g_applied > 0, "no-fall-damage", [] { RemoveAll(); });
        }

        void Menu() override {
            ImGui::TextDisabled("host only; the game's own fall immunity, on each player at their first landing");
            ImGui::TextDisabled("active on %d character(s)", g_applied.load());
        }
    } g_feature;
}

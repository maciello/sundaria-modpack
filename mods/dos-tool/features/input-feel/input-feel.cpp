#include "feature.hpp"
#include "input-feel.hpp"
#include "imgui.h"

#include <Windows.h>
#include <atomic>
#include <cstdint>
#include <string>
#include "Engine_classes.hpp"
#include "Archon_classes.hpp"
#include "GameplayAbilities_classes.hpp"
#include "GameplayAbilities_parameters.hpp"
#include "Archon_parameters.hpp"
#include "BP_PlayerControllerGame_classes.hpp"
#include "BP_GameplayAnimNotify_classes.hpp"

// Input feel: Overwatch-style ability cancels. Pressing a different ability while the current one
// holds the animation lock:
//   windup (effect not fired yet) -> the current ability is cancelled, the new one starts now
//   out    (effect already fired) -> only the recovery animation is cut, the new one starts now
// The game's input queue then fires the press immediately instead of when the lock ends.
// "Out" = the montage's ApplyEffect/ShootProjectile notify fired for the animating ability.
// A windup cancel also removes the cooldown the cancelled ability already started.
// Game thread only (ProcessEvent listener). Removing the anim-lock and cooldown effects is server-side:
// works solo and as host; as a co-op client the host decides.
using namespace SDK;

namespace {
    inline bool PtrOk(const void* p) {
        const uintptr_t v = reinterpret_cast<uintptr_t>(p);
        return v > 0x10000 && v < 0x7FFFFFFFFFFFull;
    }

    input_feel::Options g_opt;  // written by the menu (render thread), read on the game thread
    std::atomic<bool> g_on{false};

    // Resolved once on the render thread before the listener goes live.
    UFunction* g_fnNotify = nullptr;       // UBP_GameplayAnimNotify_C::Received_Notify
    UFunction* g_fnCheckLock = nullptr;    // UArchonAbilitySystemComponent::CheckAnimLock
    UFunction* g_fnRemoveLock = nullptr;   // UArchonAbilitySystemComponent::RemoveAnimLock
    UFunction* g_fnCancel = nullptr;       // UGameplayAbility::K2_CancelAbility
    UFunction* g_fnInputPressed = nullptr; // UArchonGameplayAbility::GetInputPressed
    UFunction* g_fnAbilityInput[32] = {};  // ABP_PlayerControllerGame_C::InpActEvt_Ability<N>_* (press + release)
    int g_numAbilityInput = 0;

    // Game thread state.
    UGameplayAbility* g_outAbility = nullptr;  // animating ability whose effect already fired
    std::atomic<int> g_presses{0}, g_windupCancels{0}, g_recoveryCuts{0}, g_outs{0}, g_refunds{0};
    std::atomic<bool> g_queueOff{false};
    thread_local bool t_busy = false;  // our own UFunction calls re-enter ProcessEvent

    // Same call shape as Dumper-7's generated bodies.
    void Call(const UObject* obj, UFunction* fn, void* parms) {
        auto flags = fn->FunctionFlags;
        fn->FunctionFlags |= 0x400;  // FUNC_Native
        obj->ProcessEvent(fn, parms);
        fn->FunctionFlags = flags;
    }
    bool CallBool(const UObject* obj, UFunction* fn) {
        struct { bool ret; } p{};
        Call(obj, fn, &p);
        return p.ret;
    }

    bool Resolve() {
        UClass* pc = ABP_PlayerControllerGame_C::StaticClass();
        UClass* notify = UBP_GameplayAnimNotify_C::StaticClass();
        UClass* asc = UArchonAbilitySystemComponent::StaticClass();
        if (!PtrOk(pc) || !PtrOk(notify) || !PtrOk(asc)) return false;
        g_fnNotify = notify->GetFunction("BP_GameplayAnimNotify_C", "Received_Notify");
        g_fnCheckLock = asc->GetFunction("ArchonAbilitySystemComponent", "CheckAnimLock");
        g_fnRemoveLock = asc->GetFunction("ArchonAbilitySystemComponent", "RemoveAnimLock");
        g_fnCancel = UGameplayAbility::StaticClass()->GetFunction("GameplayAbility", "K2_CancelAbility");
        g_fnInputPressed = UArchonGameplayAbility::StaticClass()->GetFunction("ArchonGameplayAbility", "GetInputPressed");
        g_numAbilityInput = 0;
        for (UField* f = pc->Children; PtrOk(f) && g_numAbilityInput < 32; f = f->Next)
            if (f->GetName().starts_with("InpActEvt_Ability") && !f->GetName().starts_with("InpActEvt_AbilityBar")
                && !f->GetName().starts_with("InpActEvt_AbilityModifier"))
                g_fnAbilityInput[g_numAbilityInput++] = static_cast<UFunction*>(f);
        return g_fnNotify && g_fnCheckLock && g_fnRemoveLock && g_fnCancel && g_fnInputPressed
            && g_numAbilityInput > 0;
    }

    AArchonCharacter* LocalHero(APlayerController** outPc) {
        UWorld* w = UWorld::GetWorld();
        if (!PtrOk(w) || !PtrOk(w->OwningGameInstance)) return nullptr;
        auto& lps = w->OwningGameInstance->LocalPlayers;
        if (lps.Num() <= 0 || !PtrOk(lps[0])) return nullptr;
        APlayerController* pc = lps[0]->PlayerController;
        if (!PtrOk(pc) || !PtrOk(pc->Pawn) || !pc->Pawn->IsA(AArchonCharacter::StaticClass())) return nullptr;
        *outPc = pc;
        return static_cast<AArchonCharacter*>(pc->Pawn);
    }

    // Removes the cooldown effect(s) this ability applied (core finds them by class + effect context).
    void RefundCooldown(UAbilitySystemComponent* asc, UGameplayAbility* ab) {
        game::EffectRef fx[8];
        const int n = game::CooldownEffects(asc, ab, fx, 8);  // collect first: removing reshapes the array
        for (int i = 0; i < n; i++) if (game::RemoveEffect(asc, fx[i].handle)) g_refunds++;
    }

    bool IsAbilityInput(UFunction* fn) {
        for (int i = 0; i < g_numAbilityInput; i++) if (g_fnAbilityInput[i] == fn) return true;
        return false;
    }

    void OnEvent(void* objp, void* fnp, void* parms) {
        UFunction* fn = static_cast<UFunction*>(fnp);
        if (t_busy || !g_on.load(std::memory_order_relaxed)) return;
        const bool notify = fn == g_fnNotify;
        if (!notify && !IsAbilityInput(fn)) return;

        APlayerController* pc = nullptr;
        AArchonCharacter* hero = LocalHero(&pc);
        if (!hero) return;
        UArchonAbilitySystemComponent* asc = hero->mAbilitySystemComponent;
        if (!PtrOk(asc)) return;
        UGameplayAbility* cur = asc->LocalAnimMontageInfo.AnimatingAbility;
        if (!PtrOk(cur)) cur = nullptr;

        t_busy = true;
        if (notify) {
            // Received_Notify(MeshComp, Animation): the effect of our animating ability fired.
            auto* n = static_cast<UBP_GameplayAnimNotify_C*>(objp);
            auto* mesh = *static_cast<USkeletalMeshComponent**>(parms);
            const auto t = n->mGameplayAnimNotifyType;
            if (cur && mesh == hero->Mesh
                && (t == EGameplayAnimNotifyType::ApplyEffect || t == EGameplayAnimNotifyType::ShootProjectile)) {
                g_outAbility = cur;
                g_outs++;
            }
        } else if (objp == pc && pc->IsInputKeyDown(*static_cast<FKey*>(parms))) {  // press, not release
            g_presses++;
            if (auto* gpc = static_cast<ABP_PlayerControllerGame_C*>(pc); pc->IsA(ABP_PlayerControllerGame_C::StaticClass()))
                g_queueOff = gpc->bDisableInputQueue;
            const bool locked = CallBool(asc, g_fnCheckLock);
            // The game already ran this press: if it was for the animating ability, its input reads pressed.
            const bool same = cur && cur->IsA(UArchonGameplayAbility::StaticClass()) && CallBool(cur, g_fnInputPressed);
            const auto phase = !cur ? input_feel::Phase::Idle
                             : cur == g_outAbility ? input_feel::Phase::Out : input_feel::Phase::Windup;
            const input_feel::Action a = input_feel::Decide(phase, locked, same, g_opt);
            if (a.cancelCurrent) { Call(cur, g_fnCancel, nullptr); g_windupCancels++; }
            if (a.refundCooldown) RefundCooldown(asc, cur);
            if (a.removeLock) { Call(asc, g_fnRemoveLock, nullptr); if (!a.cancelCurrent) g_recoveryCuts++; }
        }
        t_busy = false;
    }

    struct InputFeel : feature::Feature {
        bool resolved = false;

        InputFeel() : Feature("Input feel", feature::Stage::Alpha) { optIn = true; }  // new game-thread hook

        void OnFrame(const feature::Frame&) override {
            if (!resolved && !(resolved = Resolve())) return;
            if (!g_on) { g_on = true; game::SetEventListener(&OnEvent, true); }
        }

        void Off() override {
            g_on = false;
            game::SetEventListener(&OnEvent, false);
            g_outAbility = nullptr;
        }

        void Menu() override {
            ImGui::Checkbox("Cancel windup with another ability", &g_opt.cancelWindup);
            ImGui::Checkbox("Cut recovery once the ability is out", &g_opt.cancelRecovery);
            ImGui::Checkbox("Refund cooldown of a cancelled ability", &g_opt.refundCooldown);
            if (!resolved) { ImGui::TextDisabled("waiting for game classes"); return; }
            ImGui::TextDisabled("presses %d  out %d  windup cancels %d  refunds %d  recovery cuts %d", g_presses.load(),
                                g_outs.load(), g_windupCancels.load(), g_refunds.load(), g_recoveryCuts.load());
            if (g_queueOff) ImGui::TextWrapped("Note: the game setting 'Disable input queue' is on. Turn it off: cancels need the queue.");
        }
    } g_input_feel;
}

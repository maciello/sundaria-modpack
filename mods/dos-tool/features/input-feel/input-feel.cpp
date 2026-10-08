#include "feature.hpp"
#include "input-feel.hpp"
#include "logger.hpp"
#include "ref.hpp"
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

    // Game thread. Blueprint classes come and go with the map (#63): ref::Fn / ref::Cached re-resolve them.
    ref::Fn g_fnNotify{UBP_GameplayAnimNotify_C::StaticClass, "BP_GameplayAnimNotify_C", "Received_Notify"};
    ref::Fn g_fnCheckLock{UArchonAbilitySystemComponent::StaticClass, "ArchonAbilitySystemComponent", "CheckAnimLock"};
    ref::Fn g_fnRemoveLock{UArchonAbilitySystemComponent::StaticClass, "ArchonAbilitySystemComponent", "RemoveAnimLock"};
    ref::Fn g_fnCancel{UGameplayAbility::StaticClass, "GameplayAbility", "K2_CancelAbility"};
    ref::Fn g_fnInputPressed{UArchonGameplayAbility::StaticClass, "ArchonGameplayAbility", "GetInputPressed"};
    ref::Cached<UClass> g_pcCls{[] { return ABP_PlayerControllerGame_C::StaticClass(); }};
    ref::Ref g_inputFrom;           // the controller class g_abilityInput was read from; its functions die with it
    ref::Ref g_abilityInput[32];    // its InpActEvt_Ability<N>_* (press + release)
    int g_numAbilityInput = 0;
    std::atomic<bool> g_ready{false};  // the notify class was found once (menu)

    // Game thread state.
    ref::Ref g_outAbility;  // animating ability whose effect already fired
    std::atomic<int> g_presses{0}, g_windupCancels{0}, g_recoveryCuts{0}, g_outs{0}, g_refunds{0};
    std::atomic<bool> g_queueOff{false};
    thread_local bool t_busy = false;  // our own UFunction calls re-enter ProcessEvent

    // Same call shape as Dumper-7's generated bodies.
    void Call(const UObject* obj, UFunction* fn, void* parms) {
        if (!fn) return;
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

    // O(1) while the controller class lives; re-read (O(its functions)) once per class load.
    bool IsAbilityInput(const void* fn) {
        UClass* pc = g_pcCls.Get();
        if (!pc) return false;
        if (!(g_inputFrom == g_pcCls.r)) {
            g_inputFrom = g_pcCls.r;
            g_numAbilityInput = 0;
            for (UField* f = pc->Children; PtrOk(f) && g_numAbilityInput < 32; f = f->Next)
                if (f->GetName().starts_with("InpActEvt_Ability") && !f->GetName().starts_with("InpActEvt_AbilityBar")
                    && !f->GetName().starts_with("InpActEvt_AbilityModifier"))
                    g_abilityInput[g_numAbilityInput++] = ref::Ref(f);
        }
        for (int i = 0; i < g_numAbilityInput; i++) if (g_abilityInput[i].ptr == fn) return true;  // valid: same class as g_inputFrom
        return false;
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

    void OnEvent(void* objp, void* fnp, void* parms) {
        UFunction* fn = static_cast<UFunction*>(fnp);
        if (t_busy || !g_on.load(std::memory_order_relaxed) || !game::OnGameThread()) return;  // shared state, UFunction calls and ref resolution: game thread only
        UFunction* notifyFn = g_fnNotify.Get();  // O(1); null until a world loads the class
        if (!notifyFn) return;
        g_ready = true;
        const bool notify = fn == notifyFn;
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
            // Received_Notify(MeshComp, Animation): the effect of our animating ability fired. Trust boundary: the
            // engine's call, so check what it passed (a stale function pointer once gave parms = null, #63).
            auto* n = static_cast<UBP_GameplayAnimNotify_C*>(objp);
            const bool ok = parms && PtrOk(n) && n->IsA(UBP_GameplayAnimNotify_C::StaticClass());
            auto* mesh = ok ? *static_cast<USkeletalMeshComponent**>(parms) : nullptr;
            const auto t = ok ? n->mGameplayAnimNotifyType : EGameplayAnimNotifyType{};
            if (ok && cur && mesh == hero->Mesh
                && (t == EGameplayAnimNotifyType::ApplyEffect || t == EGameplayAnimNotifyType::ShootProjectile)) {
                g_outAbility = ref::Ref(cur);
                g_outs++;
            }
        } else if (objp == pc && parms && pc->IsInputKeyDown(*static_cast<FKey*>(parms))) {  // press, not release
            g_presses++;
            if (auto* gpc = static_cast<ABP_PlayerControllerGame_C*>(pc); pc->IsA(ABP_PlayerControllerGame_C::StaticClass()))
                g_queueOff = gpc->bDisableInputQueue;
            const bool locked = CallBool(asc, g_fnCheckLock.Get());
            // The game already ran this press: if it was for the animating ability, its input reads pressed.
            const bool same = cur && cur->IsA(UArchonGameplayAbility::StaticClass()) && CallBool(cur, g_fnInputPressed.Get());
            const auto phase = !cur ? input_feel::Phase::Idle
                             : g_outAbility.Is(cur) ? input_feel::Phase::Out : input_feel::Phase::Windup;
            const input_feel::Action a = input_feel::Decide(phase, locked, same, g_opt);
            if (a.cancelCurrent) { Call(cur, g_fnCancel.Get(), nullptr); g_windupCancels++; }
            if (a.refundCooldown) RefundCooldown(asc, cur);
            if (a.removeLock) { Call(asc, g_fnRemoveLock.Get(), nullptr); if (!a.cancelCurrent) g_recoveryCuts++; }
            if (a.cancelCurrent || a.removeLock)
                logger::log(std::string("[input-feel] ") + (a.cancelCurrent ? "windup cancel " : "recovery cut ") + (cur ? cur->Class->GetName() : std::string("?"))
                            + (a.refundCooldown ? ", cooldown refunded" : ""));
        }
        t_busy = false;
    }

    struct InputFeel : feature::Feature {
        InputFeel() : Feature("Input feel", feature::Stage::Alpha) { optIn = true; }  // new game-thread hook

        void OnFrame(const feature::Frame&) override {
            if (!g_on) { g_on = true; game::SetEventListener(&OnEvent, true); }
        }

        void Off() override {
            g_on = false;
            game::SetEventListener(&OnEvent, false);
            g_outAbility = {};
        }

        void Menu() override {
            ImGui::Checkbox("Cancel windup with another ability", &g_opt.cancelWindup);
            ImGui::Checkbox("Cut recovery once the ability is out", &g_opt.cancelRecovery);
            ImGui::Checkbox("Refund cooldown of a cancelled ability", &g_opt.refundCooldown);
            if (!g_ready) { ImGui::TextDisabled("waiting for game classes"); return; }
            ImGui::TextDisabled("presses %d  out %d  windup cancels %d  refunds %d  recovery cuts %d", g_presses.load(),
                                g_outs.load(), g_windupCancels.load(), g_refunds.load(), g_recoveryCuts.load());
            if (g_queueOff) ImGui::TextWrapped("Note: the game setting 'Disable input queue' is on. Turn it off: cancels need the queue.");
        }
    } g_input_feel;
}

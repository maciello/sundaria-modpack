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
#include "BP_PlayerControllerGame_parameters.hpp"
#include "BP_GameplayAnimNotify_classes.hpp"

// Input feel: Overwatch-style ability cancels. A different ability that fails to start because the current one
// holds the animation lock:
//   windup (effect not fired yet) -> the current ability is cancelled, the new one starts on the next tick
//   out    (effect already fired) -> only the recovery animation is cut, the new one starts on the next tick
// Trigger: the controller's K2_OnAbilityFailed with AbilityFailureReason.AnimLock. The game raises it when its input
// queue (FlushInputs, every tick) tries the press; the queue retries the press next tick, now without the lock.
// The press event itself (InpActEvt_Ability*) only enqueues, so it cannot tell which ability the press is for.
// Hold abilities: pressing B releases A first (ProcessAbilityInput), A's release then blocks B, so B cancels A.
// "Out" = the montage's ApplyEffect/ShootProjectile notify fired in this montage play.
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
    ref::Fn g_fnFailed{ABP_PlayerControllerGame_C::StaticClass, "BP_PlayerControllerGame_C", "K2_OnAbilityFailed"};
    ref::Fn g_fnActivated{AArchonPlayerController::StaticClass, "ArchonPlayerController", "K2_OnAbilityActivated"};  // no BP override
    ref::Cached<UClass> g_pcCls{[] { return ABP_PlayerControllerGame_C::StaticClass(); }};
    ref::Ref g_inputFrom;           // the controller class g_abilityInput was read from; its functions die with it
    ref::Ref g_abilityInput[32];    // its InpActEvt_Ability<N>_* (press + release)
    int g_numAbilityInput = 0;
    std::atomic<bool> g_ready{false};  // the notify class was found once (menu)

    // Game thread state.
    ref::Ref g_outAbility, g_outMontage;  // the montage play whose effect already fired
    bool g_outBit = false;
    input_feel::Gate g_gate;  // one action per real key press
    ref::Ref g_cancelled;     // g_gate.cancelled, kept as a live-checked ref
    std::atomic<int> g_presses{0}, g_blocks{0}, g_windupCancels{0}, g_recoveryCuts{0}, g_outs{0}, g_refunds{0}, g_ghosts{0};
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
        const bool failed = !notify && g_fnFailed.Is(fn);
        const bool activated = !notify && !failed && g_fnActivated.Is(fn);
        if (!notify && !failed && !activated) {
            if (IsAbilityInput(fn) && parms && PtrOk(objp)) {
                auto* pc = static_cast<APlayerController*>(objp);
                if (pc->IsInputKeyDown(*static_cast<FKey*>(parms))) { g_presses++; g_gate.Press(); }  // press, not release
            }
            return;
        }

        APlayerController* pc = nullptr;
        AArchonCharacter* hero = LocalHero(&pc);
        if (!hero) return;
        UArchonAbilitySystemComponent* asc = hero->mAbilitySystemComponent;
        if (!PtrOk(asc)) return;
        const auto& info = asc->LocalAnimMontageInfo;
        UGameplayAbility* cur = PtrOk(info.AnimatingAbility) ? info.AnimatingAbility : nullptr;
        const input_feel::Play play{cur, cur ? info.AnimMontage : nullptr, info.PlayBit};

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
                g_outMontage = ref::Ref(info.AnimMontage);
                g_outBit = info.PlayBit;
                g_outs++;
            }
        } else if (activated && objp == pc && parms) {
            // K2_OnAbilityActivated(Handle, Ability): the ability we cancelled restarting with no new press is a
            // stale queue copy of its old press (the refund passes its cooldown check): cancel it again, bounded.
            auto* p = static_cast<Params::ArchonPlayerController_K2_OnAbilityActivated*>(parms);
            UGameplayAbility* started = PtrOk(p->Ability) ? p->Ability : nullptr;
            g_gate.cancelled = g_cancelled.Get<void>();
            if (started && g_gate.IsGhost(started)) {
                g_gate.ghosts++; g_ghosts++;
                Call(started, g_fnCancel.Get(), nullptr);
                if (g_opt.refundCooldown) RefundCooldown(asc, started);
                logger::log("[input-feel] ghost " + started->Class->GetName() + " restarted from the queue without a press: cancelled");
            }
        } else if (failed && objp == pc && parms && pc->IsA(ABP_PlayerControllerGame_C::StaticClass())) {
            // K2_OnAbilityFailed(Handle, Ability, FailureReason): the game already ran its handler (queued the retry).
            auto* p = static_cast<Params::BP_PlayerControllerGame_C_K2_OnAbilityFailed*>(parms);
            g_queueOff = static_cast<ABP_PlayerControllerGame_C*>(pc)->bDisableInputQueue;
            UGameplayAbility* pressed = PtrOk(p->Ability) ? p->Ability : nullptr;
            // Queue retries (every tick, also stale copies) never act: only the first failure after a real press.
            // ponytail: the press is not matched to its ability, so a stale queued press failing first in the tick after a
            // real press takes its turn; match by input id (FindAbilityInputIDFromHandle vs the pressed slot) if that shows up.
            if (pressed && cur && g_gate.MayAct()) {
                bool animLock = false;
                for (const auto& tag : p->FailureReason.GameplayTags)
                    if (tag.TagName.ToString() == "AbilityFailureReason.AnimLock") animLock = true;
                if (animLock) {
                    const auto phase = input_feel::PhaseOf(play, {g_outAbility.Get<UGameplayAbility>(), g_outMontage.Get<UObject>(), g_outBit});
                    const bool same = pressed == cur;  // mashing / combos: the game's own queue handles it
                    const input_feel::Action a = input_feel::Decide(phase, true, same, g_opt);
                    g_blocks++;
                    g_gate.Acted(a.cancelCurrent ? cur : nullptr);  // this press is used up, acted on or not
                    g_cancelled = a.cancelCurrent ? ref::Ref(cur) : ref::Ref{};
                    if (a.cancelCurrent) { Call(cur, g_fnCancel.Get(), nullptr); g_windupCancels++; }
                    if (a.refundCooldown) RefundCooldown(asc, cur);
                    if (a.removeLock) { Call(asc, g_fnRemoveLock.Get(), nullptr); if (!a.cancelCurrent) g_recoveryCuts++; }
                    if (a.cancelCurrent || a.removeLock) {
                        const bool free = CallBool(asc, g_fnCheckLock.Get());  // CheckAnimLock: true = no anim lock
                        const UGameplayAbility* now = PtrOk(asc->LocalAnimMontageInfo.AnimatingAbility) ? asc->LocalAnimMontageInfo.AnimatingAbility : nullptr;
                        logger::log(std::string("[input-feel] ") + (a.cancelCurrent ? "windup cancel " : "recovery cut ") + cur->Class->GetName()
                                    + " for " + pressed->Class->GetName() + (a.refundCooldown ? ", cooldown refunded" : "")
                                    + ", lock " + (free ? "gone" : "STILL ON")
                                    + (a.cancelCurrent ? std::string(", montage ") + (now == cur ? "STILL PLAYING" : "stopped") : ""));
                    } else {
                        logger::log("[input-feel] " + pressed->Class->GetName() + " blocked by anim lock of " + cur->Class->GetName()
                                    + (same ? " (same ability)" : phase == input_feel::Phase::Out ? " (out, cut off)" : " (windup, cancel off)"));
                    }
                }
            }
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
            g_outAbility = {}; g_outMontage = {}; g_cancelled = {}; g_gate = {};
        }

        void Menu() override {
            ImGui::Checkbox("Cancel windup with another ability", &g_opt.cancelWindup);
            ImGui::Checkbox("Cut recovery once the ability is out", &g_opt.cancelRecovery);
            ImGui::Checkbox("Refund cooldown of a cancelled ability", &g_opt.refundCooldown);
            if (!g_ready) { ImGui::TextDisabled("waiting for game classes"); return; }
            ImGui::TextDisabled("presses %d  out %d  lock blocks %d  windup cancels %d  refunds %d  recovery cuts %d  ghosts %d", g_presses.load(),
                                g_outs.load(), g_blocks.load(), g_windupCancels.load(), g_refunds.load(), g_recoveryCuts.load(), g_ghosts.load());
            if (g_queueOff) ImGui::TextWrapped("Note: the game setting 'Disable input queue' is on. Turn it off: cancels need the queue.");
        }
    } g_input_feel;
}

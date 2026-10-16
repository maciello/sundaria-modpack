#include "feature.hpp"
#include "roll-cancel.hpp"
#include "drain.hpp"
#include "logger.hpp"
#include "ref.hpp"
#include "umg.hpp"
#include "imgui.h"
#include "imgui_internal.h"  // MarkIniSettingsDirty

#include <Windows.h>
#include <atomic>
#include <cctype>
#include <cstdio>
#include <string>
#include "Engine_classes.hpp"
#include "Archon_classes.hpp"
#include "GameplayAbilities_classes.hpp"
#include "BP_PlayerControllerGame_classes.hpp"
#include "BP_CharacterBase_classes.hpp"
#include "BP_CharacterBase_parameters.hpp"
#include "BP_GameAbility_Dodge_classes.hpp"

// Roll cancel (#12): a jump or an ability press ends the combat roll at once (maintainer: "Sofort") and the game's own
// jump / ability runs. Why the roll blocks a jump: JumpPressed asks the pawn I_CheckActionAllowed(0), which refuses
// while a root-motion montage plays unless CanDodge is set (the game opens it with BP_AnimNotifyState_DodgeWindow ->
// I_DodgeWindowOpen). So, before the game handles the press (event filter): open that window, cancel the dodge ability
// (its montage ends with it) and remove its anim lock. The window is closed again on the next world ticks.
// Game thread only (filter, world tick, Drain). Host / solo: the server decides ability state in co-op.
using namespace SDK;

namespace {
    bool PtrOk(const void* p) {
        const uintptr_t v = reinterpret_cast<uintptr_t>(p);
        return v > 0x10000 && v < 0x7FFFFFFFFFFFull;
    }

    std::atomic<bool> g_on{false}, g_jump{true}, g_abilities{true};
    std::atomic<int> g_jumpCancels{0}, g_abilityCancels{0};

    ref::Cached<UClass> g_pcCls{[] { return ABP_PlayerControllerGame_C::StaticClass(); }};
    ref::Cached<UClass> g_dodgeCls{[] { return UBP_GameAbility_Dodge_C::StaticClass(); }};
    ref::Fn g_fnWindow{ABP_CharacterBase_C::StaticClass, "BP_CharacterBase_C", "I_DodgeWindowOpen"};
    ref::Fn g_fnCancel{UGameplayAbility::StaticClass, "GameplayAbility", "K2_CancelAbility"};
    ref::Fn g_fnRemoveLock{UArchonAbilitySystemComponent::StaticClass, "ArchonAbilitySystemComponent", "RemoveAnimLock"};

    // The controller's key events, read once per class load: InpActEvt_Jump* / InpActEvt_Ability<N>* (press + release).
    struct Input { ref::Ref fn; roll_cancel::Input kind; };
    ref::Ref g_inputFrom;
    Input g_input[48];
    int g_numInput = 0;

    roll_cancel::Input KindOf(const void* fn) {
        UClass* pc = g_pcCls.Get();
        if (!pc) return roll_cancel::Input::Other;
        if (!(g_inputFrom == g_pcCls.r)) {
            g_inputFrom = g_pcCls.r;
            g_numInput = 0;
            for (UField* f = pc->Children; PtrOk(f) && g_numInput < 48; f = f->Next) {
                const std::string n = f->GetName();
                constexpr char kAb[] = "InpActEvt_Ability";
                if (n.starts_with("InpActEvt_Jump")) g_input[g_numInput++] = {ref::Ref(f), roll_cancel::Input::Jump};  // Jump, JumpInteract
                else if (n.starts_with(kAb) && std::isdigit(static_cast<unsigned char>(n[sizeof(kAb) - 1])))
                    g_input[g_numInput++] = {ref::Ref(f), roll_cancel::Input::Ability};
            }
        }
        for (int i = 0; i < g_numInput; i++) if (g_input[i].fn.ptr == fn) return g_input[i].kind;  // same class as g_inputFrom
        return roll_cancel::Input::Other;
    }

    thread_local bool t_busy = false;
    ref::Ref g_windowOn;          // the character whose dodge window we opened
    ULONGLONG g_windowCloseAt = 0;

    void Window(UObject* hero, bool open) {
        UFunction* fn = g_fnWindow.Get();
        if (!fn || !PtrOk(hero) || !hero->IsA(ABP_CharacterBase_C::StaticClass())) return;
        Params::BP_CharacterBase_C_I_DodgeWindowOpen p{};
        p.DodgeWindowOpen = open;
        hero->ProcessEvent(fn, &p);
    }

    void CloseWindow() {
        if (UObject* h = g_windowOn.Get<UObject>()) Window(h, false);
        g_windowOn = {};
    }

    // Before the game runs a key event: end the roll if this press should. Never swallows the press.
    bool Filter(void* objp, void* fnp, void* parms) {
        if (t_busy || !g_on.load(std::memory_order_relaxed) || !game::OnGameThread() || !parms || !PtrOk(objp)) return false;
        const roll_cancel::Input in = KindOf(fnp);
        if (in == roll_cancel::Input::Other) return false;
        APlayerController* pc = umg::LocalPC();
        if (objp != pc || !PtrOk(pc->Pawn) || !pc->Pawn->IsA(AArchonCharacter::StaticClass())) return false;
        auto* hero = static_cast<AArchonCharacter*>(pc->Pawn);
        UArchonAbilitySystemComponent* asc = hero->mAbilitySystemComponent;
        UClass* dodgeCls = g_dodgeCls.Get();
        if (!PtrOk(asc) || !dodgeCls) return false;
        // The roll is recognised by its montage: in game the dodge's montage plays with no AnimatingAbility set
        // (log 2026-10-16: "animating=- montage=Mon_Dodge_H_M playing=1"), so the ability can't tell us.
        UGameplayAbility* animating = asc->LocalAnimMontageInfo.AnimatingAbility;
        UAnimMontage* montage = asc->LocalAnimMontageInfo.AnimMontage;
        UAnimInstance* ai = PtrOk(hero->Mesh) ? hero->Mesh->AnimScriptInstance : nullptr;
        const bool dodgeMontage = PtrOk(montage) && montage->GetName().starts_with("Mon_Dodge");
        const bool rolling = PtrOk(ai) && (dodgeMontage || (PtrOk(animating) && animating->IsA(dodgeCls))) && PtrOk(montage)
                             && ai->Montage_IsPlaying(montage);
        t_busy = true;
        const bool press = pc->IsInputKeyDown(*static_cast<FKey*>(parms));
        const roll_cancel::Options o{g_jump.load(), g_abilities.load()};
        if (roll_cancel::Cancel(rolling, press, in, o)) {
            if (in == roll_cancel::Input::Jump) {  // the jump check lets a press through while the window is open
                Window(hero, true);
                g_windowOn = ref::Ref(hero);
                g_windowCloseAt = GetTickCount64() + 150;
                g_jumpCancels++;
            } else {
                g_abilityCancels++;
            }
            if (PtrOk(animating) && animating->IsA(dodgeCls))  // the ability, when the game does name it
                if (UFunction* fn = g_fnCancel.Get()) umg::CallNative(animating, fn, nullptr);
            ai->Montage_Stop(0.1f, montage);  // ends the roll's root motion: the jump check passes, the ability can start
            if (UFunction* fn = g_fnRemoveLock.Get()) umg::CallNative(asc, fn, nullptr);
            logger::log(in == roll_cancel::Input::Jump ? "[roll-cancel] roll ended by jump" : "[roll-cancel] roll ended by ability");
        }
        t_busy = false;
        return false;
    }

    void OnTick(void*, void*, void*) {
        if (t_busy || !game::OnGameThread() || !g_windowOn.ptr || GetTickCount64() < g_windowCloseAt) return;
        t_busy = true;
        CloseWindow();
        t_busy = false;
    }

    game::Drain g_drain;  // Off(): the game thread closes a window we left open

    struct RollCancel : feature::Feature {
        bool listening = false;
        RollCancel() : Feature("Roll cancel", feature::Stage::Alpha) {}

        void OnFrame(const feature::Frame&) override {
            if (listening) return;
            listening = true;
            g_on = true;
            game::SetEventFilter(&Filter, true);
            game::OnWorldTick(&OnTick, true);
        }

        void Off() override {
            g_on = false;
            if (listening) {
                game::SetEventFilter(&Filter, false);
                game::OnWorldTick(&OnTick, false);
            }
            listening = false;
            g_drain.Request(true, "roll-cancel", [] { CloseWindow(); });
        }

        void Menu() override {
            bool j = g_jump, a = g_abilities;
            if (ImGui::Checkbox("Jump ends the roll", &j)) { g_jump = j; ImGui::MarkIniSettingsDirty(); }
            if (ImGui::Checkbox("Attacks and abilities end the roll", &a)) { g_abilities = a; ImGui::MarkIniSettingsDirty(); }
            ImGui::TextDisabled("rolls ended: %d by jump, %d by ability", g_jumpCancels.load(), g_abilityCancels.load());
        }

        void Load(const char* key, const char* value) override {
            const std::string k = key;
            if (k == "jump") g_jump = std::atoi(value) != 0;
            else if (k == "abilities") g_abilities = std::atoi(value) != 0;
        }
        void Save(std::vector<std::pair<std::string, std::string>>& out) override {
            out.push_back({"jump", g_jump ? "1" : "0"});
            out.push_back({"abilities", g_abilities ? "1" : "0"});
        }
    } g_rollCancel;
}

#include "hub_ui.hpp"
#include "game.hpp"
#include "logger.hpp"
#include "ref.hpp"

#include <Windows.h>
#include <atomic>
#include <cstdio>
#include <string>
#include <vector>
#include "Engine_classes.hpp"

using namespace SDK;

namespace {
    bool PtrOk(const void* p) {
        const uintptr_t v = reinterpret_cast<uintptr_t>(p);
        return v > 0x10000 && v < 0x7FFFFFFFFFFFull;
    }

    APlayerController* LocalPC() {
        UWorld* w = UWorld::GetWorld();
        if (!PtrOk(w) || !PtrOk(w->OwningGameInstance) || w->OwningGameInstance->LocalPlayers.Num() < 1) return nullptr;
        ULocalPlayer* lp = w->OwningGameInstance->LocalPlayers[0];
        return PtrOk(lp) && PtrOk(lp->PlayerController) ? lp->PlayerController : nullptr;
    }

    bool StartsWith(const std::string& s, const char* p) { return s.rfind(p, 0) == 0; }

    // UFunction by name anywhere up the class chain (BP interface events live on the BP class)
    UFunction* FindFn(UClass* cls, const char* name) {
        for (const UStruct* c = cls; PtrOk(c); c = c->SuperStruct)
            for (UField* f = c->Children; PtrOk(f); f = f->Next)
                if (f->HasTypeFlag(EClassCastFlags::Function) && f->GetName() == name) return static_cast<UFunction*>(f);
        return nullptr;
    }

    // render thread → game thread
    std::atomic<int> g_hideReq{-1};        // -1 none, 0 show, 1 hide
    std::atomic<bool> g_talkReq{false};
    float g_talkAt[3] = {};                // written before g_talkReq is set
    std::atomic<int> g_focusReq{-1};       // -1 none, 0 unfocus, 1 focus g_focusAt
    float g_focusAt[3] = {};
    ref::Ref g_focused;                    // game thread
    std::atomic<int> g_rim{-1};            // the hub outline's custom depth stencil, learnt from a click zone
    std::atomic<bool> g_listening{false};

    // game thread
    std::vector<ref::Ref> g_hidden;        // what we hid (restored only if still alive)
    ref::Ref g_hiddenWorld;

    template <class F> void ForEachButton(UWorld* w, F&& fn) {
        for (int li = 0; li < w->Levels.Num(); li++) {
            ULevel* lvl = w->Levels[li];
            if (!PtrOk(lvl)) continue;
            for (int ai = 0; ai < lvl->Actors.Num(); ai++) {
                AActor* a = lvl->Actors[ai];
                if (PtrOk(a) && PtrOk(a->Class) && StartsWith(a->Class->GetName(), "BP_TriggerVolumeButton")) fn(a);
            }
        }
    }

    void ApplyHide(bool hide) {  // once per toggle: walks the levels' actors, not per frame
        UWorld* w = UWorld::GetWorld();
        if (!PtrOk(w)) return;
        if (!g_hiddenWorld.Is(w)) { g_hidden.clear(); g_hiddenWorld = ref::Ref(w); }
        if (!hide) {
            for (const ref::Ref& r : g_hidden)
                if (auto* a = r.Get<AActor>()) { a->SetActorHiddenInGame(false); a->SetActorEnableCollision(true); }
            logger::log("[hub-ui] hub buttons back: " + std::to_string(g_hidden.size()));
            g_hidden.clear();
            return;
        }
        ForEachButton(w, [](AActor* a) {
            // NPC and world map zones stay (they live in the tavern now); hub-hidden ones aren't ours to bring back
            if (a->bHidden || a->Class->GetName() == "BP_TriggerVolumeButton_Character_C" || a->GetName().rfind("Button_Map_", 0) == 0) return;
            a->SetActorHiddenInGame(true);
            a->SetActorEnableCollision(false);
            g_hidden.push_back(ref::Ref(a));
            logger::log("[hub-ui] hidden " + a->Class->GetName() + " " + a->GetName());
        });
        logger::log("[hub-ui] hub buttons hidden: " + std::to_string(g_hidden.size()));
    }

    AActor* NpcButtonNear(UWorld* w, const float at[3]) {
        AActor* best = nullptr;
        float bestD = 500.0f * 500.0f;
        ForEachButton(w, [&](AActor* a) {
            const bool zone = a->Class->GetName() == "BP_TriggerVolumeButton_Character_C" || a->GetName().rfind("Button_Map_", 0) == 0;
            if (!zone || !PtrOk(a->RootComponent)) return;
            const FVector p = a->RootComponent->RelativeLocation;
            const float dx = p.X - at[0], dy = p.Y - at[1], dz = p.Z - at[2];
            const float d = dx * dx + dy * dy + dz * dz;
            if (d < bestD) { bestD = d; best = a; }
        });
        return best;
    }

    void SetFocused(AActor* button, APlayerController* pc, bool on) {
        UFunction* fn = FindFn(button->Class, "I_HubTriggerSetGamepadFocused");
        if (!PtrOk(fn)) return;
        alignas(16) unsigned char parms[256] = {};  // { APlayerController* PlayerController; bool IsFocused; }
        *reinterpret_cast<APlayerController**>(parms) = pc;
        parms[8] = on ? 1 : 0;
        button->ProcessEvent(fn, parms);
        // the outline (rim stencil on the NPC) comes from Show/HideSelection, which only mouse hover calls
        if (UFunction* sel = FindFn(button->Class, on ? "ShowSelection" : "HideSelection"); PtrOk(sel)) {
            alignas(16) unsigned char sp[256] = {};  // { int32 ControllerId; } local player 0
            button->ProcessEvent(sel, sp);
        }
    }

    void ApplyFocus(bool on) {
        UWorld* w = UWorld::GetWorld();
        APlayerController* pc = LocalPC();
        if (!PtrOk(w) || !PtrOk(pc)) return;
        AActor* next = on ? NpcButtonNear(w, g_focusAt) : nullptr;
        AActor* prev = g_focused.Get<AActor>();
        if (next == prev) return;
        if (prev) SetFocused(prev, pc, false);
        g_focused = ref::Ref(next);
        if (next) {
            SetFocused(next, pc, true);
            if (UFunction* rim = FindFn(next->Class, "I_RimShaderOwnerGetStencil"); PtrOk(rim) && g_rim.load() < 0) {
                alignas(16) unsigned char rp[512] = {};  // { bool VisualizeRim; bool NeedsViewCheck; int32 RimStencil @4; TSets… }
                next->ProcessEvent(rim, rp);              // ponytail: the two out TSets' engine memory is leaked once
                g_rim = *reinterpret_cast<int32_t*>(rp + 4);
                logger::log("[hub-ui] outline stencil " + std::to_string(g_rim.load()));
            }
        }
    }

    void ApplyTalk() {
        UWorld* w = UWorld::GetWorld();
        APlayerController* pc = LocalPC();
        if (!PtrOk(w) || !PtrOk(pc)) return;
        AActor* best = NpcButtonNear(w, g_talkAt);
        if (!best) { logger::log("[hub-ui] talk: no NPC click zone near"); return; }
        UFunction* fn = FindFn(best->Class, "I_HubTriggerGamepadSelect");
        if (!PtrOk(fn)) { logger::log("[hub-ui] talk: " + best->GetName() + " has no I_HubTriggerGamepadSelect"); return; }
        alignas(16) unsigned char parms[256] = {};  // { APlayerController* PlayerController; … BP locals }
        *reinterpret_cast<APlayerController**>(parms) = pc;
        best->ProcessEvent(fn, parms);
        logger::log("[hub-ui] talk: pressed " + best->GetName());
    }

    void OnEvent(void*, void*, void*) {
        if (!game::OnGameThread()) return;
        const int h = g_hideReq.exchange(-1);
        if (h >= 0) ApplyHide(h == 1);
        const int fo = g_focusReq.exchange(-1);
        if (fo >= 0) ApplyFocus(fo == 1);
        if (g_talkReq.exchange(false)) ApplyTalk();
    }

    void Listen() { if (!g_listening.exchange(true)) game::OnGameTick(OnEvent, true); }
}

void hub_ui::SetButtonsHidden(bool hidden) { g_hideReq = hidden ? 1 : 0; Listen(); }

void hub_ui::Focus(bool on, float x, float y, float z) {
    g_focusAt[0] = x; g_focusAt[1] = y; g_focusAt[2] = z;
    g_focusReq = on ? 1 : 0;
    Listen();
}

void hub_ui::Talk(float x, float y, float z) {
    g_talkAt[0] = x; g_talkAt[1] = y; g_talkAt[2] = z;
    g_talkReq = true;
    Listen();
}

void hub_ui::Stop() {
    for (int i = 0; i < 50 && (g_hideReq.load() >= 0 || g_talkReq.load() || g_focusReq.load() >= 0); i++) Sleep(2);  // let queued work run
    if (g_listening.exchange(false)) game::OnGameTick(OnEvent, false);
}

int hub_ui::RimStencil() { return g_rim.load() < 0 ? 1 : g_rim.load(); }

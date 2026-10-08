#include "hub_ui.hpp"
#include "game.hpp"
#include "logger.hpp"

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
    bool Alive(const UObject* o) { return PtrOk(o) && UObject::GObjects->GetByIndex(o->Index) == o; }

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
    AActor* g_focused = nullptr;           // game thread
    std::atomic<bool> g_listening{false};

    // game thread
    std::vector<AActor*> g_hidden;         // what we hid (restored only if still alive)
    UWorld* g_hiddenWorld = nullptr;

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
        if (w != g_hiddenWorld) { g_hidden.clear(); g_hiddenWorld = w; }
        if (!hide) {
            for (AActor* a : g_hidden) if (Alive(a)) { a->SetActorHiddenInGame(false); a->SetActorEnableCollision(true); }
            logger::log("[hub-ui] hub buttons back: " + std::to_string(g_hidden.size()));
            g_hidden.clear();
            return;
        }
        ForEachButton(w, [](AActor* a) {
            if (a->bHidden || a->Class->GetName() == "BP_TriggerVolumeButton_Character_C") return;  // NPC zones stay; hub-hidden ones aren't ours
            a->SetActorHiddenInGame(true);
            a->SetActorEnableCollision(false);
            g_hidden.push_back(a);
            logger::log("[hub-ui] hidden " + a->Class->GetName() + " " + a->GetName());
        });
        logger::log("[hub-ui] hub buttons hidden: " + std::to_string(g_hidden.size()));
    }

    AActor* NpcButtonNear(UWorld* w, const float at[3]) {
        AActor* best = nullptr;
        float bestD = 500.0f * 500.0f;
        ForEachButton(w, [&](AActor* a) {
            if (a->Class->GetName() != "BP_TriggerVolumeButton_Character_C" || !PtrOk(a->RootComponent)) return;
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
    }

    void ApplyFocus(bool on) {
        UWorld* w = UWorld::GetWorld();
        APlayerController* pc = LocalPC();
        if (!PtrOk(w) || !PtrOk(pc)) return;
        AActor* next = on ? NpcButtonNear(w, g_focusAt) : nullptr;
        if (next == g_focused) return;
        if (Alive(g_focused)) SetFocused(g_focused, pc, false);
        g_focused = next;
        if (next) SetFocused(next, pc, true);
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

    void Listen() { if (!g_listening.exchange(true)) game::SetEventListener(OnEvent, true); }
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
    if (g_listening.exchange(false)) game::SetEventListener(OnEvent, false);
}

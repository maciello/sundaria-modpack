#include "npc_audio.hpp"
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

    SRWLOCK g_mu = SRWLOCK_INIT;
    std::string g_who, g_anim;            // guarded by g_mu
    float g_volume = 1.0f;
    std::atomic<bool> g_dirty{false};     // apply on the next game-thread event
    std::atomic<bool> g_listening{false};

    bool Contains(const std::string& s, const std::string& part) { return !part.empty() && s.find(part) != std::string::npos; }

    void Apply() {
        AcquireSRWLockShared(&g_mu);
        const std::string who = g_who, anim = g_anim;
        const float volume = g_volume;
        ReleaseSRWLockShared(&g_mu);
        UWorld* w = UWorld::GetWorld();
        if (!PtrOk(w)) return;
        if (who.empty()) return;
        int actors = 0, comps = 0, montages = 0;
        UClass* audioCls = UAudioComponent::StaticClass();
        for (int li = 0; li < w->Levels.Num(); li++) {  // runs once per change, not per frame
            ULevel* lvl = w->Levels[li];
            if (!PtrOk(lvl)) continue;
            for (int ai = 0; ai < lvl->Actors.Num(); ai++) {
                AActor* a = lvl->Actors[ai];
                if (!PtrOk(a) || !PtrOk(a->Class) || !a->IsA(ACharacter::StaticClass())) continue;
                if (!Contains(a->Class->GetName(), who)) continue;
                actors++;
                TArray<UActorComponent*> audio = a->K2_GetComponentsByClass(audioCls);
                for (int ci = 0; ci < audio.Num(); ci++)
                    if (PtrOk(audio[ci])) { static_cast<UAudioComponent*>(audio[ci])->SetVolumeMultiplier(volume); comps++; }
                auto* c = static_cast<ACharacter*>(a);
                if (PtrOk(c->Mesh)) {  // what is it playing? (where the hammer sound might come from)
                    UAnimInstance* inst = c->Mesh->AnimScriptInstance;
                    UAnimMontage* m = PtrOk(inst) ? inst->GetCurrentActiveMontage() : nullptr;
                    UAnimationAsset* single = c->Mesh->AnimationData.AnimToPlay;
                    char ib[300];
                    std::snprintf(ib, sizeof(ib), "[npc-audio] %s: anim instance %s, montage %s, single anim %s", a->GetName().c_str(),
                                  PtrOk(inst) ? inst->Class->GetName().c_str() : "-", PtrOk(m) ? m->GetName().c_str() : "-",
                                  PtrOk(single) ? single->GetName().c_str() : "-");
                    logger::log(ib);
                }
                if (volume <= 0.001f && PtrOk(c->Mesh) && PtrOk(c->Mesh->AnimScriptInstance)) {
                    UAnimInstance* ai2 = c->Mesh->AnimScriptInstance;
                    if (PtrOk(ai2->GetCurrentActiveMontage())) { ai2->Montage_Stop(0.3f, nullptr); montages++; }
                }
            }
        }
        char buf[200];
        std::snprintf(buf, sizeof(buf), "[npc-audio] %s at %.0f%%: %d actors, %d audio components, %d montages stopped",
                      who.c_str(), volume * 100.0f, actors, comps, montages);
        logger::log(buf);
    }

    void OnEvent(void*, void*, void*) {
        if (!game::OnGameThread()) return;
        if (g_dirty.exchange(false)) Apply();
    }
}

void npc_audio::Set(const char* who, const char* anim, float volume) {
    AcquireSRWLockExclusive(&g_mu);
    g_who = who ? who : "";
    g_anim = anim ? anim : "";
    g_volume = volume;
    ReleaseSRWLockExclusive(&g_mu);
    g_dirty = true;
    if (!g_listening.exchange(true)) game::SetEventListener(OnEvent, true);
}

void npc_audio::Tick() {
    static UWorld* seen = nullptr;  // a new map (hub loaded again): apply again
    UWorld* w = UWorld::GetWorld();
    if (w != seen) { seen = w; g_dirty = true; }
    if (!g_dirty.load() && g_listening.load() && g_who.empty()) {  // restored: listener no longer needed
        g_listening = false;
        game::SetEventListener(OnEvent, false);
    }
}

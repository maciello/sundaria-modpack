#include "feature.hpp"
#include "logger.hpp"
#include "imgui.h"

#include <Windows.h>
#include <atomic>
#include <cstdio>
#include <iterator>
#include <string>
#include <vector>
#include "Engine_classes.hpp"

using namespace SDK;

// Anim test (dev): plays an animation from our own pak (~mods/SundariaAnims_P.pak, /Game/SundariaMod/Anims: Paragon
// animations retargeted onto a verified copy of the player skeleton) on the character you control, as a dynamic
// montage in one of the skeleton's slots. The game's AnimBP keeps running and armor/weapons follow the body (master
// pose); when the montage ends or Stop is pressed, everything is vanilla again. Changes nothing persistent.
namespace {
    bool PtrOk(const void* p) {
        const uintptr_t v = reinterpret_cast<uintptr_t>(p);
        return v > 0x10000 && v < 0x7FFFFFFFFFFFull;
    }

    const char* kAnims[] = {"GS_Idle", "GS_Emote_Taunt_BringItOn_T1", "GS_Emote_Master_SwordPlay_T3", "GS_Attack_PrimaryA"};

    USkeletalMeshComponent* PawnMesh() {
        UWorld* w = UWorld::GetWorld();
        if (!PtrOk(w) || !PtrOk(w->OwningGameInstance) || w->OwningGameInstance->LocalPlayers.Num() <= 0) return nullptr;
        ULocalPlayer* lp = w->OwningGameInstance->LocalPlayers[0];
        APlayerController* pc = PtrOk(lp) ? lp->PlayerController : nullptr;
        APawn* pawn = PtrOk(pc) ? pc->Pawn : nullptr;
        if (!PtrOk(pawn) || !pawn->IsA(ACharacter::StaticClass())) return nullptr;
        USkeletalMeshComponent* m = static_cast<ACharacter*>(pawn)->Mesh;
        return PtrOk(m) ? m : nullptr;
    }

    // Request from the render thread (menu) to the game thread (UFunction calls).
    enum class Req { None, Play, Stop };
    std::atomic<Req> g_req{Req::None};
    std::atomic<bool> g_done{false};
    std::string g_reqAnim;  // written before g_req is set
    FName g_reqSlot;
    bool g_reqLoop = false;
    std::string g_result;   // written on the game thread before g_done

    std::string Play() {
        USkeletalMeshComponent* m = PawnMesh();
        UAnimInstance* ai = m ? m->AnimScriptInstance : nullptr;
        if (!PtrOk(ai)) return "no character with an AnimBP: enter a dungeon, the creator, or walk the hub hero (F7)";
        const std::string path = "/Game/SundariaMod/Anims/" + g_reqAnim + "." + g_reqAnim;
        const std::wstring wpath(path.begin(), path.end());
        UObject* o = UKismetSystemLibrary::LoadAsset_Blocking(
            UKismetSystemLibrary::Conv_SoftObjPathToSoftObjRef(UKismetSystemLibrary::MakeSoftObjectPath(FString(wpath.c_str()))));
        if (!PtrOk(o) || !o->IsA(UAnimSequenceBase::StaticClass())) return "not found: " + path + " (is ~mods/SundariaAnims_P.pak installed, game restarted?)";
        UAnimMontage* mt = ai->PlaySlotAnimationAsDynamicMontage(static_cast<UAnimSequenceBase*>(o), g_reqSlot, 0.25f, 0.25f, 1.0f,
                                                                 g_reqLoop ? 1000 : 1, -1.0f, 0.0f);
        return std::string(PtrOk(mt) ? "playing " : "slot rejected it: ") + g_reqAnim + " in slot " + g_reqSlot.ToString();
    }

    std::string Stop() {
        USkeletalMeshComponent* m = PawnMesh();
        UAnimInstance* ai = m ? m->AnimScriptInstance : nullptr;
        if (!PtrOk(ai)) return "no character";
        ai->StopSlotAnimation(0.25f, g_reqSlot);
        return "stopped";
    }

    void OnEvent(void*, void*, void*) {
        if (!game::OnGameThread()) return;
        const Req r = g_req.exchange(Req::None);
        if (r == Req::None) return;
        g_result = r == Req::Play ? Play() : Stop();
        g_done = true;
    }

    struct AnimTest : feature::Feature {
        int anim = 1, slot = 0;
        bool loop = false, listening = false;
        std::string status;
        std::vector<FName> slots;
        std::vector<std::string> slotNames;

        AnimTest() : Feature("Anim test", feature::Stage::Alpha) {}

        void Ask(Req r) {
            if (slots.empty()) { status = "no slots: no character"; return; }
            g_reqAnim = kAnims[anim];
            g_reqSlot = slots[slot];
            g_reqLoop = loop;
            g_done = false;
            g_req = r;
            if (!listening) { game::OnGameTick(&OnEvent, true); listening = true; }
            status = "...";
        }

        void OnFrame(const feature::Frame&) override {
            if (listening && g_done.exchange(false)) {
                game::OnGameTick(&OnEvent, false);
                listening = false;
                status = g_result;
                logger::log(("[anim-test] " + status).c_str());
            }
        }

        void Off() override {
            if (!slots.empty() && !listening) {  // stop what we started, then unhook
                g_reqSlot = slots[slot];
                g_done = false;
                g_req = Req::Stop;
                game::OnGameTick(&OnEvent, true);
                for (int i = 0; i < 100 && !g_done; i++) Sleep(10);
            }
            game::OnGameTick(&OnEvent, false);
            listening = false;
            g_req = Req::None;
        }

        // Slot names come from the character's skeleton (memory reads): montages only play in slots its AnimBP has.
        void ReadSlots() {
            slots.clear();
            slotNames.clear();
            USkeletalMeshComponent* m = PawnMesh();
            USkeleton* sk = m && PtrOk(m->SkeletalMesh) && PtrOk(m->SkeletalMesh->Skeleton) ? m->SkeletalMesh->Skeleton : nullptr;
            if (!sk) return;
            for (const FAnimSlotGroup& g : sk->SlotGroups)
                for (const FName& n : g.SlotNames) {
                    slots.push_back(n);
                    slotNames.push_back(g.GroupName.ToString() + "." + n.ToString());
                }
            if (slot >= int(slots.size())) slot = 0;
        }

        void Menu() override {
            if (slots.empty() || ImGui::SmallButton("Refresh slots")) ReadSlots();
            if (ImGui::BeginCombo("Animation", kAnims[anim])) {
                for (int i = 0; i < int(std::size(kAnims)); i++)
                    if (ImGui::Selectable(kAnims[i], i == anim)) anim = i;
                ImGui::EndCombo();
            }
            if (ImGui::BeginCombo("Slot", slotNames.empty() ? "-" : slotNames[slot].c_str())) {
                for (int i = 0; i < int(slotNames.size()); i++)
                    if (ImGui::Selectable(slotNames[i].c_str(), i == slot)) slot = i;
                ImGui::EndCombo();
            }
            ImGui::Checkbox("Loop", &loop);
            if (ImGui::Button("Play")) Ask(Req::Play);
            ImGui::SameLine();
            if (ImGui::Button("Stop")) Ask(Req::Stop);
            ImGui::TextDisabled("%s", status.empty() ? "needs ~mods/SundariaAnims_P.pak" : status.c_str());
        }
    } g_animTest;
}

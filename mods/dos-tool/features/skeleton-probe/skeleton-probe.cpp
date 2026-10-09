#include "feature.hpp"
#include "logger.hpp"
#include "skeleton-probe.hpp"
#include "imgui.h"

#include <Windows.h>
#include <cstdint>
#include <cstdio>
#include <algorithm>
#include <atomic>
#include <cstring>
#include <string>
#include <vector>
#include "Engine_classes.hpp"
#include "BP_BipedCharacter_classes.hpp"

using namespace SDK;

// Skeleton probe (dev): a menu button writes dos-tool-bones-<character class>.txt next to the game exe - every skeletal mesh part
// of the character you control (body, head, armor slots: mesh + skeleton + master pose) and each distinct
// skeleton's bone tree with its reference pose. Question it answers: which bones (names, hierarchy, rest pose)
// would a skeleton in our UE 4.27 project need so Paragon animations can be retargeted onto the game's own one.
// Memory reads only (render thread); changes nothing in the game.
namespace {
    bool PtrOk(const void* p) {
        const uintptr_t v = reinterpret_cast<uintptr_t>(p);
        return v > 0x10000 && v < 0x7FFFFFFFFFFFull;
    }

    struct RawArray { uint8_t* data; int32_t num, max; };

    // USkeleton's FReferenceSkeleton is not in the dump (native, no UPROPERTY): it sits in the padding after
    // RefLocalPoses (0x58, unverified). FReferenceSkeleton starts with RawRefBoneInfo (FMeshBoneInfo: FName +
    // int32 parent; 12 bytes in shipping, more with editor data) followed by RawRefBonePose (FTransform, 0x30).
    // We look for that pair near 0x58 and accept it only when it forms a valid bone tree.
    bool ReadBones(USkeleton* sk, std::vector<skeleton_probe::Bone>& out, int& offset, int& stride) {
        auto* base = reinterpret_cast<uint8_t*>(sk);
        for (int off = 0x50; off <= 0x100; off += 8) {
            const auto* info = reinterpret_cast<const RawArray*>(base + off);
            const auto* pose = reinterpret_cast<const RawArray*>(base + off + 0x10);
            if (!PtrOk(info->data) || !PtrOk(pose->data) || info->num <= 0 || info->num > 2000 || info->num != pose->num ||
                info->max < info->num || pose->max < pose->num)
                continue;
            for (int st : {12, 16, 24, 32}) {
                std::vector<skeleton_probe::Bone> bones(info->num);
                bool sane = true;
                for (int i = 0; i < info->num && sane; i++) {
                    const uint8_t* e = info->data + size_t(i) * st;
                    int32_t parent;
                    std::memcpy(&parent, e + 8, 4);
                    bones[i].parent = parent;
                    sane = parent >= -1 && parent < i;
                }
                if (!sane) continue;
                for (int i = 0; i < info->num; i++) {
                    bones[i].name = reinterpret_cast<const FName*>(info->data + size_t(i) * st)->ToString();
                    const float* t = reinterpret_cast<const float*>(pose->data + size_t(i) * 0x30);  // FQuat, FVector+pad, FVector+pad
                    for (int k = 0; k < 4; k++) bones[i].q[k] = t[k];
                    for (int k = 0; k < 3; k++) bones[i].t[k] = t[4 + k], bones[i].s[k] = t[8 + k];
                }
                if (!skeleton_probe::ValidTree(bones)) continue;
                out.swap(bones);
                offset = off, stride = st;
                return true;
            }
        }
        return false;
    }

    std::string PathOf(UObject* o) {
        if (!PtrOk(o)) return "-";
        std::string pkg = "?";  // outermost outer = the package (/Game/...)
        for (UObject* p = o->Outer; PtrOk(p); p = p->Outer) pkg = p->Name.GetRawString();  // raw: FName::ToString() cuts at the last '/'
        return pkg + "." + o->GetName();
    }

    std::string Dump(std::string& status, std::string& file) {
        UWorld* w = UWorld::GetWorld();
        if (!PtrOk(w) || !PtrOk(w->OwningGameInstance) || w->OwningGameInstance->LocalPlayers.Num() <= 0) return status = "no world", "";
        ULocalPlayer* lp = w->OwningGameInstance->LocalPlayers[0];
        APlayerController* pc = PtrOk(lp) ? lp->PlayerController : nullptr;
        APawn* pawn = PtrOk(pc) ? pc->Pawn : nullptr;
        if (!PtrOk(pawn) || !pawn->IsA(ACharacter::StaticClass()))
            return status = "no character: enter a dungeon, or walk the hub hero (F7)", "";
        auto* ch = static_cast<ACharacter*>(pawn);
        file = "dos-tool-bones-" + pawn->Class->GetName() + ".txt";  // one file per character class: dumps don't overwrite each other

        std::vector<std::pair<const char*, USkeletalMeshComponent*>> parts = {{"Mesh", ch->Mesh}};
        if (pawn->IsA(ABP_BipedCharacter_C::StaticClass())) {
            auto* b = static_cast<ABP_BipedCharacter_C*>(pawn);
            parts.insert(parts.end(), {{"Body", b->MeshCompBody}, {"Head", b->MeshCompHead}, {"Hair", b->MeshCompHair},
                                       {"Beard", b->MeshCompBeard}, {"Hand", b->MeshCompHand}, {"Leg", b->MeshCompLeg},
                                       {"Foot", b->MeshCompFoot}, {"Armor_Helmet", b->MeshCompArmor_Helmet},
                                       {"Armor_Chest", b->MeshCompArmor_Chest}, {"Armor_Pauldron", b->MeshCompArmor_Pauldron},
                                       {"Armor_Bracer", b->MeshCompArmor_Bracer}, {"Armor_Glove", b->MeshCompArmor_Glove},
                                       {"Armor_Belt", b->MeshCompArmor_Belt}, {"Armor_Pants", b->MeshCompArmor_Pants},
                                       {"Armor_Greave", b->MeshCompArmor_Greave}, {"Armor_Boot", b->MeshCompArmor_Boot},
                                       {"Armor_Crown", b->MeshCompArmor_Crown}, {"Armor_Back", b->MeshCompArmor_Back}});
        }

        std::string txt = "# dos-tool skeleton probe: character " + PathOf(pawn->Class) + "\n# part  mesh  skeleton  master-pose\n";
        std::vector<USkeleton*> skeletons;
        for (auto& [name, c] : parts) {
            if (!PtrOk(c) || !c->IsA(USkeletalMeshComponent::StaticClass())) continue;
            USkeletalMesh* m = PtrOk(c->SkeletalMesh) ? c->SkeletalMesh : nullptr;
            USkeleton* sk = m && PtrOk(m->Skeleton) ? m->Skeleton : nullptr;
            UObject* master = c->MasterPoseComponent.Get();
            txt += std::string(name) + "  " + PathOf(m) + "  " + PathOf(sk) + "  " + (PtrOk(master) ? master->GetName() : "-") + "\n";
            if (sk && std::find(skeletons.begin(), skeletons.end(), sk) == skeletons.end()) skeletons.push_back(sk);
        }

        int ok = 0;
        for (USkeleton* sk : skeletons) {
            std::vector<skeleton_probe::Bone> bones;
            int off = 0, stride = 0;
            txt += "\n# skeleton " + PathOf(sk);
            if (!ReadBones(sk, bones, off, stride)) { txt += "  (bone table not found)\n"; continue; }
            char hdr[96];
            std::snprintf(hdr, sizeof(hdr), "  bones=%zu  (FReferenceSkeleton @0x%X, stride %d)\n", bones.size(), off, stride);
            txt += hdr;
            txt += "# idx parent name  t(ranslation) q(uat xyzw) s(cale), local to parent, cm\n" + skeleton_probe::Format(bones);
            ok++;
        }
        char buf[128];
        std::snprintf(buf, sizeof(buf), "%zu skeleton(s), %d bone table(s) read", skeletons.size(), ok);
        status = buf;
        return txt;
    }

    bool WriteNextToExe(const char* file, const std::string& txt) {
        char path[MAX_PATH] = {};
        GetModuleFileNameA(nullptr, path, MAX_PATH);
        std::string p = path;
        p = p.substr(0, p.find_last_of("\\/") + 1) + file;
        FILE* f = std::fopen(p.c_str(), "wb");
        if (!f) return false;
        std::fwrite(txt.data(), 1, txt.size(), f);
        std::fclose(f);
        return true;
    }

    // Verify: our copy of the game skeleton, cooked at our own path into ~mods/SundariaSkelVerify_P.pak, against the
    // game's original. Loading an asset is a UFunction call: it runs on the game thread (event listener).
    constexpr const wchar_t* kCopy = L"/Game/SundariaMod/SkelVerify/H_M_Root_Mesh_04_Skeleton.H_M_Root_Mesh_04_Skeleton";
    std::atomic<bool> g_verifyAsked{false}, g_verifyDone{false};
    std::string g_verifyResult;  // written on the game thread before g_verifyDone, read after

    USkeleton* PawnSkeleton() {
        UWorld* w = UWorld::GetWorld();
        if (!PtrOk(w) || !PtrOk(w->OwningGameInstance) || w->OwningGameInstance->LocalPlayers.Num() <= 0) return nullptr;
        ULocalPlayer* lp = w->OwningGameInstance->LocalPlayers[0];
        APlayerController* pc = PtrOk(lp) ? lp->PlayerController : nullptr;
        APawn* pawn = PtrOk(pc) ? pc->Pawn : nullptr;
        if (!PtrOk(pawn) || !pawn->IsA(ACharacter::StaticClass())) return nullptr;
        USkeletalMeshComponent* mc = static_cast<ACharacter*>(pawn)->Mesh;
        USkeletalMesh* m = PtrOk(mc) && PtrOk(mc->SkeletalMesh) ? mc->SkeletalMesh : nullptr;
        return m && PtrOk(m->Skeleton) ? m->Skeleton : nullptr;
    }

    std::string Verify() {
        USkeleton* game = PawnSkeleton();
        if (!game) return "no character: enter a dungeon, the creator, or walk the hub hero (F7)";
        UObject* o = UKismetSystemLibrary::LoadAsset_Blocking(
            UKismetSystemLibrary::Conv_SoftObjPathToSoftObjRef(UKismetSystemLibrary::MakeSoftObjectPath(FString(kCopy))));
        if (!PtrOk(o) || !o->IsA(USkeleton::StaticClass())) return "copy not found: is ~mods/SundariaSkelVerify_P.pak installed (game restarted)?";
        std::vector<skeleton_probe::Bone> a, b;
        int off = 0, st = 0;
        if (!ReadBones(game, a, off, st) || !ReadBones(static_cast<USkeleton*>(o), b, off, st)) return "bone table not readable";
        const skeleton_probe::Diff d = skeleton_probe::Compare(a, b);
        char buf[256];
        if (!d.sameOrder)
            std::snprintf(buf, sizeof(buf), "MISMATCH: %zu vs %zu bones, first differing index %d (%s vs %s)", a.size(), b.size(), d.firstMismatch,
                          d.firstMismatch < int(a.size()) ? a[d.firstMismatch].name.c_str() : "-",
                          d.firstMismatch < int(b.size()) ? b[d.firstMismatch].name.c_str() : "-");
        else
            std::snprintf(buf, sizeof(buf), "OK: %zu bones, same order; ref pose max drift %.3f cm, rotation %.6f (1-|dot|)", a.size(), d.maxT, d.maxQ);
        return buf;
    }

    void OnEvent(void*, void*, void*) {
        if (!game::OnGameThread() || !g_verifyAsked.exchange(false)) return;
        g_verifyResult = Verify();
        g_verifyDone = true;
    }

    struct SkeletonProbe : feature::Feature {
        std::string status;
        bool listening = false;
        SkeletonProbe() : Feature("Skeleton probe", feature::Stage::Alpha) {}
        void OnFrame(const feature::Frame&) override {
            if (listening && g_verifyDone.exchange(false)) {
                game::OnGameTick(&OnEvent, false);
                listening = false;
                status = "verify: " + g_verifyResult;
                logger::log(("[bones] " + status).c_str());
            }
        }
        void Off() override {
            if (listening) game::OnGameTick(&OnEvent, false);
            listening = false;
            g_verifyAsked = false;
        }
        void Menu() override {
            if (ImGui::Button("Verify skeleton copy") && !listening) {
                status = "verifying...";
                g_verifyDone = false;
                g_verifyAsked = true;
                game::OnGameTick(&OnEvent, true);
                listening = true;
            }
            ImGui::SameLine();
            if (ImGui::Button("Dump my character's bones")) {
                std::string file;
                const std::string txt = Dump(status, file);
                if (!txt.empty()) {
                    status += WriteNextToExe(file.c_str(), txt) ? " -> " + file : " (could not write file)";
                    logger::log(("[bones] " + status).c_str());
                }
            }
            ImGui::TextDisabled("%s", status.empty() ? "writes dos-tool-bones-<character>.txt next to the game exe" : status.c_str());
        }
    } g_skeletonProbe;
}

#include "feature.hpp"
#include "logger.hpp"
#include "imgui.h"

#include <Windows.h>
#include <atomic>
#include <cstdio>
#include <string>
#include <vector>
#include "Engine_classes.hpp"

using namespace SDK;

// Pose recorder (dev): records every bone of the character you control (local transform in its parent's space,
// 30 samples/s) while the GAME's own animations play, into dos-tool-pose-<character>.txt next to the game exe.
// Question it answers: how do the game's animations move the helper bones (skirt, Physique, twist) relative to the
// main bones, so retargeted animations can drive them the same way (clothes follow legs and arms).
// Reads only; UFunction calls (bone names, transforms) on the game thread.
namespace {
    bool PtrOk(const void* p) {
        const uintptr_t v = reinterpret_cast<uintptr_t>(p);
        return v > 0x10000 && v < 0x7FFFFFFFFFFFull;
    }

    USkeletalMeshComponent* PawnMesh(std::string* cls = nullptr) {
        UWorld* w = UWorld::GetWorld();
        if (!PtrOk(w) || !PtrOk(w->OwningGameInstance) || w->OwningGameInstance->LocalPlayers.Num() <= 0) return nullptr;
        ULocalPlayer* lp = w->OwningGameInstance->LocalPlayers[0];
        APlayerController* pc = PtrOk(lp) ? lp->PlayerController : nullptr;
        APawn* pawn = PtrOk(pc) ? pc->Pawn : nullptr;
        if (!PtrOk(pawn) || !pawn->IsA(ACharacter::StaticClass())) return nullptr;
        if (cls) *cls = pawn->Class->GetName();
        USkeletalMeshComponent* m = static_cast<ACharacter*>(pawn)->Mesh;
        return PtrOk(m) ? m : nullptr;
    }

    constexpr ULONGLONG kStepMs = 33;
    std::atomic<bool> g_rec{false}, g_done{false};
    ULONGLONG g_until = 0, g_next = 0;
    std::string g_cls, g_error;
    std::vector<FName> g_names;
    std::vector<std::string> g_nameStr;
    std::vector<float> g_data;  // per sample: per bone qx qy qz qw tx ty tz
    int g_samples = 0;

    void Sample() {
        const ULONGLONG now = GetTickCount64();
        if (now < g_next) return;
        g_next = now + kStepMs;
        USkeletalMeshComponent* m = PawnMesh(g_names.empty() ? &g_cls : nullptr);
        if (!m) { g_error = "character gone"; g_rec = false; g_done = true; return; }
        if (g_names.empty()) {
            const int n = m->GetNumBones();
            for (int i = 0; i < n; i++) {
                g_names.push_back(m->GetBoneName(i));
                g_nameStr.push_back(g_names.back().ToString());
            }
        }
        for (const FName& n : g_names) {
            const FTransform t = m->GetSocketTransform(n, ERelativeTransformSpace::RTS_ParentBoneSpace);
            g_data.insert(g_data.end(), {t.Rotation.X, t.Rotation.Y, t.Rotation.Z, t.Rotation.W, t.Translation.X, t.Translation.Y, t.Translation.Z});
        }
        g_samples++;
        if (now >= g_until) { g_rec = false; g_done = true; }
    }

    void OnEvent(void*, void*, void*) {
        if (game::OnGameThread() && g_rec.load()) Sample();
    }

    std::string Write() {
        char path[MAX_PATH] = {};
        GetModuleFileNameA(nullptr, path, MAX_PATH);
        std::string p = path;
        const std::string file = "dos-tool-pose-" + g_cls + ".txt";
        p = p.substr(0, p.find_last_of("\\/") + 1) + file;
        FILE* f = std::fopen(p.c_str(), "wb");
        if (!f) return "could not write " + file;
        std::fprintf(f, "# dos-tool pose recorder: %s, %zu bones, %d samples every %llu ms; per bone: qx qy qz qw tx ty tz (parent space)\nbones",
                     g_cls.c_str(), g_names.size(), g_samples, kStepMs);
        for (const std::string& n : g_nameStr) std::fprintf(f, " %s", n.c_str());
        std::fprintf(f, "\n");
        const size_t row = g_names.size() * 7;
        for (int s = 0; s < g_samples; s++) {
            for (size_t k = 0; k < row; k++) std::fprintf(f, k ? " %.5g" : "%.5g", g_data[s * row + k]);
            std::fprintf(f, "\n");
        }
        std::fclose(f);
        char buf[160];
        std::snprintf(buf, sizeof(buf), "%d samples x %zu bones -> %s", g_samples, g_names.size(), file.c_str());
        return buf;
    }

    struct PoseRecorder : feature::Feature {
        int seconds = 20;
        bool listening = false;
        std::string status;

        PoseRecorder() : Feature("Pose recorder", feature::Stage::Alpha) {}

        void OnFrame(const feature::Frame&) override {
            if (listening && g_done.exchange(false)) {
                game::SetEventListener(&OnEvent, false);
                listening = false;
                status = g_error.empty() ? Write() : g_error;
                logger::log(("[pose-recorder] " + status).c_str());
            }
        }

        void Off() override {
            g_rec = false;
            if (listening) game::SetEventListener(&OnEvent, false);
            listening = false;
        }

        void Menu() override {
            ImGui::SliderInt("Seconds", &seconds, 5, 60);
            if (!listening && ImGui::Button("Record")) {
                g_names.clear(); g_nameStr.clear(); g_data.clear(); g_error.clear();
                g_samples = 0; g_next = 0; g_until = GetTickCount64() + ULONGLONG(seconds) * 1000;
                g_done = false; g_rec = true;
                game::SetEventListener(&OnEvent, true);
                listening = true;
                status = "recording: walk, run, jump, attack, turn...";
            }
            ImGui::TextDisabled("%s", status.empty() ? "records the game's own animations (dungeon or hub hero F7)" : status.c_str());
        }
    } g_poseRecorder;
}

#include "feature.hpp"
#include "imgui.h"
#include "game.hpp"
#include "logger.hpp"
#include "ref.hpp"
#include "fx.hpp"
#include "idle-loot.hpp"
#include "../shared/loot.hpp"

#include <Windows.h>
#include <atomic>
#include <cstdio>
#include <string>
#include <unordered_map>
#include <vector>

// Idle loot sparkle (#27, #110): WoW lootable-corpse style. Each unlooted loot actor carries the game particle system
// kTemplate (core/fx.hpp), tinted with the best item grade around it. Game thread, on loot events (BeginPlay,
// trigger/loot-ready/pickup/end-play): no per-frame work. Spec: references/design-system.md § Loot marker (idle sparkle).
using namespace idle_loot;

namespace {
    constexpr const char* kOwner = "idle-loot";
    struct Spark {
        ref::Ref actor;
        fx::Id id;
        int grade;
        Tuning applied;  // the tuning this component was spawned / tinted with
    };
    std::unordered_map<std::uintptr_t, Spark> g_sparks;  // key = loot actor address; game thread only
    std::atomic<bool> g_on{false};
    std::atomic<bool> g_previewAsk{false};  // Insert menu -> game thread
    std::vector<fx::Id> g_preview;           // game thread
    double g_previewEnd = 0;
    thread_local bool t_busy = false;
    // Insert-menu tuning (render thread writes, game thread reads); g_dirty = re-reconcile and restart the preview.
    std::atomic<std::size_t> g_tpl{0};
    std::atomic<float> g_scale{1}, g_bright{1}, g_height{0};
    std::atomic<bool> g_byGrade{true}, g_dirty{false};
    Tuning Cur() { return {g_tpl.load(), g_scale.load(), g_bright.load(), g_height.load(), g_byGrade.load()}; }
    // Events after which a loot actor's state may have changed (each reaches ProcessEvent: native event, bound delegate
    // or received RPC/RepNotify); on any class (Blueprint overrides declare their own), filtered to loot in OnLootEvent.
    constexpr const char* kWatch[] = {"OnTriggerChanged", "OnLootReady", "OnRep_LootIsReady", "MulticastPlayPickupSound",
                                      "I_SetInactiveLoot", "ReceiveEndPlay"};
    std::uint32_t g_version = ~0u;
    double g_settleUntil = 0;

    double Now() {
        LARGE_INTEGER t, f;
        QueryPerformanceCounter(&t);
        QueryPerformanceFrequency(&f);
        return double(t.QuadPart) / double(f.QuadPart);
    }

    void Tint(Spark& s, int grade, const std::array<style::Rgba, 8>& tiers, const Tuning& t) {
        const wchar_t* param = kTemplates[t.tpl].colour;
        if (*param) {  // templates without a colour parameter keep their own look
            const Rgb c = Emissive(grade, tiers, t);
            fx::MaterialColor(s.id, 0, param, c.r, c.g, c.b);
        }
        s.grade = grade;
        s.applied = t;
    }

    // Spawn on unlooted loot, recolour on grade change, remove from looted or gone loot. O(tracked loot).
    void Reconcile() {
        std::vector<loot::Actor> all;
        loot::Each(all);
        const std::vector<int> grades = PileGrades(all);
        std::array<style::Rgba, 8> tiers;
        if (!loot::GradeColors(tiers))
            for (int g = 0; g < 8; g++) tiers[g] = style::rarity::Of(g);
        const Tuning cur = Cur();
        std::unordered_map<std::uintptr_t, Spark> keep;
        for (size_t i = 0; i < all.size(); i++) {
            if (grades[i] == kLooted) continue;
            void* a = reinterpret_cast<void*>(all[i].id);  // live: Each read it on this game-thread call
            auto it = g_sparks.find(all[i].id);
            if (it != g_sparks.end() && it->second.actor.Is(a) && fx::Alive(it->second.id) && !NeedsRespawn(it->second.applied, cur)) {
                if (it->second.grade != grades[i] || !(it->second.applied == cur)) Tint(it->second, grades[i], tiers, cur);
                keep.emplace(it->first, it->second);
                g_sparks.erase(it);
                continue;
            }
            const bool chest = all[i].kind == loot::Kind::Chest;
            fx::Place p;
            p.z = (chest ? kLiftChest : kLiftItem) + cur.height;
            p.scale = (chest ? kScaleChest : kScaleItem) * cur.scale;
            p.cull = kCull;
            Spark s{ref::Ref(a), fx::Attach(kOwner, kTemplates[cur.tpl].path, a, p), -3, cur};
            if (!s.id) continue;
            Tint(s, grades[i], tiers, cur);
            keep.emplace(all[i].id, s);
            char b[120];
            std::snprintf(b, sizeof b, "[idle-loot] sparkle on %s %llx, grade %d", chest ? "chest" : "item", (unsigned long long)all[i].id, grades[i]);
            logger::log(std::string(b) + " " + fx::Describe(s.id));
        }
        for (auto& [k, s] : g_sparks) {  // looted, streamed out or destroyed
            fx::Remove(s.id);
            logger::log("[idle-loot] sparkle off " + std::to_string(k));
        }
        g_sparks.swap(keep);
    }

    void StopPreview() {
        for (fx::Id id : g_preview) fx::Remove(id);
        g_preview.clear();
    }

    // One sparkle per grade, a row across the hero's view at kPreviewAhead.
    void StartPreview() {
        float x, y, z, fx_, fy_;
        if (!game::LocalPawn(x, y, z, fx_, fy_)) return;
        StopPreview();
        std::array<style::Rgba, 8> tiers;
        if (!loot::GradeColors(tiers))
            for (int g = 0; g < 8; g++) tiers[g] = style::rarity::Of(g);
        const Tuning cur = Cur();
        for (int g = 0; g < kPreviewGrades; g++) {
            const float side = (g - (kPreviewGrades - 1) * 0.5f) * kPreviewGap;
            fx::Place p;
            p.x = x + fx_ * kPreviewAhead - fy_ * side, p.y = y + fy_ * kPreviewAhead + fx_ * side, p.z = z - 60 + kLiftItem + cur.height;
            p.scale = kScaleItem * cur.scale, p.cull = kCull;
            const fx::Id id = fx::At(kOwner, kTemplates[cur.tpl].path, p);
            if (!id) continue;
            Spark sp{ref::Ref(), id, g, cur};
            Tint(sp, g, tiers, cur);
            g_preview.push_back(id);
            logger::log("[idle-loot] preview grade " + std::to_string(g) + " " + fx::Describe(id));
        }
        g_previewEnd = Now() + kPreviewFor;
    }

    void OnLootEvent(void* obj, void*, void*) {  // a watched function ran: re-read loot state for kSettle
        if (game::OnGameThread() && !t_busy && loot::IsLootActor(obj)) g_settleUntil = Now() + kSettle;
    }

    void OnBeginPlay(void* obj, void* fn, void*) {
        if (!t_busy) loot::OnEvent(obj, fn);  // the tracker adds loot actors (Version() bumps)
    }

    void OnTick(void* obj, void* fn, void*) {
        if (t_busy || !game::OnGameThread()) return;
        const bool tuned = g_dirty.exchange(false);
        if (g_previewAsk.exchange(false) || (tuned && !g_preview.empty())) StartPreview();
        if (!g_preview.empty() && Now() >= g_previewEnd) StopPreview();
        loot::OnEvent(obj, fn);  // tracker: world scan on a new world, grade colours
        if (!tuned && loot::Version() == g_version && Now() >= g_settleUntil) return;
        t_busy = true;
        g_version = loot::Version();
        Reconcile();
        t_busy = false;
    }

    void Listen(bool on) {
        game::On(nullptr, "ReceiveBeginPlay", &OnBeginPlay, on);
        for (const char* fn : kWatch) game::On(nullptr, fn, &OnLootEvent, on);
        game::OnWorldTick(&OnTick, on);
    }

    struct IdleLoot : feature::Feature {
        IdleLoot() : Feature("Idle loot sparkle", feature::Stage::Alpha) { optIn = true; }

        void OnFrame(const feature::Frame&) override {
            if (g_on.load()) return;
            g_version = ~0u;
            g_on = true;
            Listen(true);
        }

        // Sparkle picker (#133): curated list, or type to search all kTemplates; sliders retune the live sparkles.
        void Menu() override {
            static char find[48] = "";
            Tuning t = Cur();
            bool changed = false;
            if (ImGui::BeginCombo("Sparkle", kTemplates[t.tpl].name)) {
                ImGui::InputText("##find", find, sizeof find);
                for (std::size_t i = 0; i < kTemplateCount; i++) {
                    if (*find ? !Contains(kTemplates[i].name, find) : i >= kCurated) continue;
                    if (ImGui::Selectable(kTemplates[i].name, i == t.tpl)) t.tpl = i, changed = true;
                }
                ImGui::EndCombo();
            }
            changed |= ImGui::SliderFloat("Size", &t.scale, 0.2f, 5.0f, "%.2fx");
            changed |= ImGui::SliderFloat("Brightness", &t.bright, 0.05f, 5.0f, "%.2fx", ImGuiSliderFlags_Logarithmic);
            changed |= ImGui::SliderFloat("Height", &t.height, -30.0f, 150.0f, "%+.0f cm");
            changed |= ImGui::Checkbox("Grade colour", &t.byGrade);
            if (changed) {
                g_tpl = t.tpl, g_scale = t.scale, g_bright = t.bright, g_height = t.height, g_byGrade = t.byGrade;
                g_dirty = true;
            }
            if (!*kTemplates[t.tpl].colour) ImGui::TextDisabled("no colour parameter: brightness/grade colour not applied");
            if (ImGui::Button("Preview sparkle")) g_previewAsk = true;  // needs the feature on: the world tick serves it
            ImGui::SameLine();
            if (ImGui::Button("Copy values")) logger::log("[idle-loot] values " + Format(t));
        }

        void Off() override {
            g_previewAsk = false;
            Listen(false);
            g_preview.clear();  // fx::Release destroys them
            fx::Release(kOwner);  // the next world tick destroys our components
            g_on = false;
            g_sparks.clear();
            loot::Reset();
        }
    } g_idle_loot;
}

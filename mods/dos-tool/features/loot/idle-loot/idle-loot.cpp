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
        std::vector<fx::Id> ids;  // one per layer (Layers / Plan order)
        int grade;
        Tuning applied;  // the tuning this was spawned / tinted with
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
    std::atomic<bool> g_byGrade{true}, g_layered{true}, g_dirty{false};
    Tuning Cur() { return {g_layered.load(), g_tpl.load(), g_scale.load(), g_bright.load(), g_height.load(), g_byGrade.load()}; }
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

    // What to spawn for a grade: the grade layers, or the one picked template (scale 1; the caller adds the kind's size).
    std::size_t Plan(const Tuning& t, int grade, Layer (&out)[3]) {
        if (t.layered) return Layers(grade, out);
        out[0] = {t.tpl, 1, 1, 0};
        return 1;
    }

    void Tint(Spark& s, int grade, const std::array<style::Rgba, 8>& tiers, const Tuning& t) {
        Layer ly[3];
        const std::size_t n = Plan(t, grade, ly);
        for (std::size_t i = 0; i < n && i < s.ids.size(); i++) {
            const wchar_t* param = kTemplates[ly[i].tpl].colour;
            if (!*param) continue;  // templates without a colour parameter keep their own look
            Tuning lt = t;
            lt.bright *= ly[i].bright * kTemplates[ly[i].tpl].gain;
            const Rgb c = Emissive(grade, tiers, lt);
            fx::MaterialColor(s.ids[i], 0, param, c.r, c.g, c.b);
            for (const wchar_t* e : kTemplates[ly[i].tpl].extra)
                if (e) fx::MaterialColor(s.ids[i], 0, e, c.r, c.g, c.b);
        }
        s.grade = grade;
        s.applied = t;
    }

    bool AllAlive(const Spark& s) {
        for (fx::Id id : s.ids) if (!fx::Alive(id)) return false;
        return !s.ids.empty();
    }

    // Spawn the layers of one pile marker via spawn(Place, path) -> Id; offsets from `at` (x, y, z) when !attach.
    template <class F> void SpawnLayers(const Tuning& t, int grade, bool chest, float kindLift, F&& spawn, std::vector<fx::Id>& ids) {
        Layer ly[3];
        const std::size_t n = Plan(t, grade, ly);
        const float kind = t.layered ? (chest ? 1.5f : 1.0f) : (chest ? kScaleChest : kScaleItem);
        for (std::size_t i = 0; i < n; i++) {
            fx::Place p;
            p.z = kindLift + t.height + ly[i].dz;
            p.scale = ly[i].scale * kind * t.scale;
            p.cull = kCull;
            if (const fx::Id id = spawn(p, kTemplates[ly[i].tpl].path)) ids.push_back(id);
        }
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
            if (it != g_sparks.end() && it->second.actor.Is(a) && AllAlive(it->second) && !NeedsRespawn(it->second.applied, cur) &&
                (!cur.layered || it->second.grade == grades[i])) {  // layered: another grade = other layers = respawn
                if (it->second.grade != grades[i] || !(it->second.applied == cur)) Tint(it->second, grades[i], tiers, cur);
                keep.emplace(it->first, it->second);
                g_sparks.erase(it);
                continue;
            }
            const bool chest = all[i].kind == loot::Kind::Chest;
            Spark s{ref::Ref(a), {}, grades[i], cur};
            SpawnLayers(cur, grades[i], chest, chest ? kLiftChest : kLiftItem,
                        [&](const fx::Place& p, const wchar_t* path) { return fx::Attach(kOwner, path, a, p); }, s.ids);
            if (s.ids.empty()) continue;
            Tint(s, grades[i], tiers, cur);
            keep.emplace(all[i].id, s);
            char b[120];
            std::snprintf(b, sizeof b, "[idle-loot] sparkle on %s %llx, grade %d", chest ? "chest" : "item", (unsigned long long)all[i].id, grades[i]);
            logger::log(std::string(b) + " layers=" + std::to_string(s.ids.size()) + " " + fx::Describe(s.ids[0]));
        }
        for (auto& [k, s] : g_sparks) {  // looted, streamed out or destroyed
            for (fx::Id id : s.ids) fx::Remove(id);
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
            const float px = x + fx_ * kPreviewAhead - fy_ * side, py = y + fy_ * kPreviewAhead + fx_ * side;
            Spark sp{ref::Ref(), {}, g, cur};
            SpawnLayers(cur, g, false, z - 60 + kLiftItem,
                        [&](fx::Place p, const wchar_t* path) { p.x = px, p.y = py; return fx::At(kOwner, path, p); }, sp.ids);
            if (sp.ids.empty()) continue;
            Tint(sp, g, tiers, cur);
            g_preview.insert(g_preview.end(), sp.ids.begin(), sp.ids.end());
            logger::log("[idle-loot] preview grade " + std::to_string(g) + " layers=" + std::to_string(sp.ids.size()) + " " + fx::Describe(sp.ids[0]));
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
            changed |= ImGui::Checkbox("Grade layers (glow + fireflies, #135)", &t.layered);
            if (!t.layered && ImGui::BeginCombo("Sparkle", kTemplates[t.tpl].name)) {
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
                g_layered = t.layered, g_tpl = t.tpl, g_scale = t.scale, g_bright = t.bright, g_height = t.height, g_byGrade = t.byGrade;
                g_dirty = true;
            }
            if (!t.layered && !*kTemplates[t.tpl].colour) ImGui::TextDisabled("no colour parameter: brightness/grade colour not applied");
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

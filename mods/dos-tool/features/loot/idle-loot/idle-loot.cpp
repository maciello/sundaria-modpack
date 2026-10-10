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

// Idle loot sparkle (#27, #110, #135): WoW lootable-corpse style. Each unlooted loot actor carries the particle layers
// Plan() gives for its grade (sparkle-config.hpp: glow + fireflies, all editable in the Insert menu), tinted with the best
// item grade around it. Game thread, on loot events (BeginPlay, trigger/loot-ready/pickup/end-play): no per-frame work.
// Spec: references/design-system.md § Loot marker (idle sparkle).
using namespace idle_loot;

namespace {
    constexpr const char* kOwner = "idle-loot";
    struct Spark {
        ref::Ref actor;
        std::vector<fx::Id> ids;  // one per plan entry (0 = that layer failed to spawn)
        std::vector<Layer> plan;
        int grade;
        Config applied;           // the settings it was tinted with
    };
    std::unordered_map<std::uintptr_t, Spark> g_sparks;  // key = loot actor address; game thread only
    std::atomic<bool> g_on{false};
    std::atomic<bool> g_previewAsk{false};  // Insert menu -> game thread
    std::vector<fx::Id> g_preview;           // game thread
    double g_previewEnd = 0;
    thread_local bool t_busy = false;
    // Insert-menu settings: the render thread edits, the game thread reads a copy. g_dirty = re-reconcile + restart the preview.
    SRWLOCK g_cfgMu = SRWLOCK_INIT;
    Config g_cfg = Defaults();
    std::atomic<bool> g_dirty{false};
    Config Cur() {
        AcquireSRWLockShared(&g_cfgMu);
        const Config c = g_cfg;
        ReleaseSRWLockShared(&g_cfgMu);
        return c;
    }
    void SetCfg(const Config& c) {
        AcquireSRWLockExclusive(&g_cfgMu);
        g_cfg = c;
        ReleaseSRWLockExclusive(&g_cfgMu);
        g_dirty = true;
    }
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

    void Tint(Spark& s, int grade, const std::array<style::Rgba, 8>& tiers, const Config& c) {
        for (std::size_t i = 0; i < s.plan.size() && i < s.ids.size(); i++) {
            const Template& t = kTemplates[s.plan[i].tpl];
            if (!s.ids[i] || !*t.colour) continue;  // templates without a colour parameter keep their own look
            const Rgb rgb = Emissive(grade, tiers, c.byGrade, s.plan[i].bright * t.gain);
            fx::MaterialColor(s.ids[i], 0, t.colour, rgb.r, rgb.g, rgb.b);
            for (const wchar_t* e : t.extra)
                if (e) fx::MaterialColor(s.ids[i], 0, e, rgb.r, rgb.g, rgb.b);
        }
        s.grade = grade;
        s.applied = c;
    }

    bool AllAlive(const Spark& s) {
        bool any = false;
        for (fx::Id id : s.ids) {
            if (!id) continue;
            if (!fx::Alive(id)) return false;
            any = true;
        }
        return any;
    }

    // Spawn the plan's components via spawn(Place, path) -> Id.
    template <class F> void SpawnPlan(const std::vector<Layer>& plan, F&& spawn, std::vector<fx::Id>& ids) {
        for (const Layer& l : plan) {
            fx::Place p;
            p.x = l.x, p.y = l.y, p.z = l.z + l.dz;  // world position: fx::At, nothing attached to the loot actor (#132)
            p.scale = l.scale;
            p.cull = kCull;
            ids.push_back(spawn(p, kTemplates[l.tpl].path));
        }
    }

    // Spawn on unlooted loot, recolour on change, remove from looted or gone loot. O(tracked loot).
    void Reconcile() {
        std::vector<loot::Actor> all;
        loot::Each(all);
        const std::vector<int> grades = PileGrades(all);
        std::array<style::Rgba, 8> tiers;
        if (!loot::GradeColors(tiers))
            for (int g = 0; g < 8; g++) tiers[g] = style::rarity::Of(g);
        const Config cur = Cur();
        std::unordered_map<std::uintptr_t, Spark> keep;
        for (size_t i = 0; i < all.size(); i++) {
            if (grades[i] == kLooted) continue;
            void* a = reinterpret_cast<void*>(all[i].id);  // live: Each read it on this game-thread call
            const bool chest = all[i].kind == loot::Kind::Chest;
            const std::vector<Layer> plan = Plan(cur, grades[i], chest, all[i].x, all[i].y, all[i].z);
            if (plan.empty()) continue;  // nothing to show for this grade: an old spark of it is removed below
            auto it = g_sparks.find(all[i].id);
            if (it != g_sparks.end() && it->second.actor.Is(a) && AllAlive(it->second) && SameSpawn(it->second.plan, plan)) {
                it->second.plan = plan;  // tint-only changes (brightness)
                if (it->second.grade != grades[i] || !(it->second.applied == cur)) Tint(it->second, grades[i], tiers, cur);
                keep.emplace(it->first, it->second);
                g_sparks.erase(it);
                continue;
            }
            Spark s{ref::Ref(a), {}, plan, grades[i], cur};
            SpawnPlan(plan, [&](const fx::Place& p, const wchar_t* path) { return fx::At(kOwner, path, p); }, s.ids);
            fx::Id first = 0;
            for (fx::Id id : s.ids) if (!first) first = id;
            if (!first) continue;
            Tint(s, grades[i], tiers, cur);
            keep.emplace(all[i].id, s);
            char b[120];
            std::snprintf(b, sizeof b, "[idle-loot] sparkle on %s %llx, grade %d layers=%zu", chest ? "chest" : "item", (unsigned long long)all[i].id,
                          grades[i], plan.size());
            logger::log(std::string(b) + " " + fx::Describe(first));
        }
        for (auto& [k, s] : g_sparks) {  // looted, streamed out, destroyed or re-laid out
            for (fx::Id id : s.ids) if (id) fx::Remove(id);
            logger::log("[idle-loot] sparkle off " + std::to_string(k));
        }
        g_sparks.swap(keep);
    }

    void StopPreview() {
        for (fx::Id id : g_preview) fx::Remove(id);
        g_preview.clear();
    }

    // One pile marker per grade, a row across the hero's view at kPreviewAhead.
    void StartPreview() {
        float x, y, z, fx_, fy_;
        if (!game::LocalPawn(x, y, z, fx_, fy_)) return;
        StopPreview();
        std::array<style::Rgba, 8> tiers;
        if (!loot::GradeColors(tiers))
            for (int g = 0; g < 8; g++) tiers[g] = style::rarity::Of(g);
        const Config cur = Cur();
        for (int g = 0; g < kPreviewGrades; g++) {
            const float side = (g - (kPreviewGrades - 1) * 0.5f) * kPreviewGap;
            const float px = x + fx_ * kPreviewAhead - fy_ * side, py = y + fy_ * kPreviewAhead + fx_ * side;
            Spark sp{ref::Ref(), {}, Plan(cur, g, false, px, py, z - 60), g, cur};
            SpawnPlan(sp.plan, [&](const fx::Place& p, const wchar_t* path) { return fx::At(kOwner, path, p); }, sp.ids);
            Tint(sp, g, tiers, cur);
            fx::Id first = 0;
            for (fx::Id id : sp.ids) if (id) { g_preview.push_back(id); if (!first) first = id; }
            logger::log("[idle-loot] preview grade " + std::to_string(g) + " layers=" + std::to_string(sp.plan.size()) +
                        (first ? " " + fx::Describe(first) : " (none for this grade)"));
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

    // One layer's editor: on/off, particle system (search all), size + copies per grade, brightness, height, lowest grade.
    bool LayerUi(const char* title, LayerCfg& l, char (&find)[48]) {
        bool ch = false;
        ImGui::PushID(title);
        if (ImGui::CollapsingHeader(title, ImGuiTreeNodeFlags_DefaultOpen)) {
            ch |= ImGui::Checkbox("On", &l.on);
            if (ImGui::BeginCombo("System", kTemplates[l.tpl].name)) {
                ImGui::InputText("##find", find, sizeof find);  // empty = the curated list, else search all
                for (std::size_t i = 0; i < kTemplateCount; i++) {
                    if (*find ? !Contains(kTemplates[i].name, find) : i >= kCurated && i != l.tpl) continue;
                    if (ImGui::Selectable(kTemplates[i].name, i == l.tpl)) l.tpl = i, ch = true;
                }
                ImGui::EndCombo();
            }
            if (!*kTemplates[l.tpl].colour) ImGui::TextDisabled("no colour parameter: brightness / grade colour not applied");
            ImGui::Text("size / copies per grade");
            for (int g = 0; g < kGrades; g++) {
                ImGui::PushID(g);
                ImGui::SetNextItemWidth(160);
                ch |= ImGui::SliderFloat("##size", &l.size[g], 0.05f, 4.0f, "%.2fx");
                ImGui::SameLine();
                ImGui::SetNextItemWidth(70);
                ch |= ImGui::SliderInt("##copies", &l.copies[g], 0, kMaxCopies);
                ImGui::SameLine();
                ImGui::TextUnformatted(kGradeName[g]);
                ImGui::PopID();
            }
            ch |= ImGui::SliderFloat("Brightness", &l.bright, 0.02f, 50.0f, "%.2fx", ImGuiSliderFlags_Logarithmic);
            ch |= ImGui::SliderFloat("Height", &l.height, -30.0f, 150.0f, "%+.0f cm");
            ch |= ImGui::Combo("Lowest grade", &l.minGrade, kGradeName, kGrades);
        }
        ImGui::PopID();
        return ch;
    }

    struct IdleLoot : feature::Feature {
        IdleLoot() : Feature("Idle loot sparkle", feature::Stage::Alpha) { optIn = true; }

        void OnFrame(const feature::Frame&) override {
            if (g_on.load()) return;
            g_version = ~0u;
            g_on = true;
            Listen(true);
        }

        // Sparkle editor (#133, #135): two layers, live on real loot and on the preview.
        void Menu() override {
            static char findGlow[48] = "", findFly[48] = "";
            Config c = Cur();
            bool ch = false;
            ch |= LayerUi("Glow layer", c.glow, findGlow);
            ch |= LayerUi("Firefly layer", c.fly, findFly);
            ch |= ImGui::Checkbox("Grade colour", &c.byGrade);
            if (ImGui::Button("Reset to defaults")) c = Defaults(), ch = true;
            if (ch) SetCfg(c);
            if (ImGui::Button("Preview sparkle")) g_previewAsk = true;  // needs the feature on: the world tick serves it
            ImGui::SameLine();
            if (ImGui::Button("Copy values")) logger::log("[idle-loot] values " + Format(c));
        }

        void Off() override {
            g_previewAsk = false;
            Listen(false);
            g_preview.clear();  // fx::Release parks them
            fx::Release(kOwner);  // the next world tick parks our components
            g_on = false;
            g_sparks.clear();
            loot::Reset();
        }
    } g_idle_loot;
}

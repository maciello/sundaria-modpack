#include "feature.hpp"
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
    };
    std::unordered_map<std::uintptr_t, Spark> g_sparks;  // key = loot actor address; game thread only
    std::atomic<bool> g_on{false};
    thread_local bool t_busy = false;
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

    void Tint(Spark& s, int grade, const std::array<style::Rgba, 8>& tiers) {
        const Rgb c = Glow(grade, tiers);
        fx::MaterialColor(s.id, 0, kGlowParam, c.r, c.g, c.b);
        s.grade = grade;
    }

    // Spawn on unlooted loot, recolour on grade change, remove from looted or gone loot. O(tracked loot).
    void Reconcile() {
        std::vector<loot::Actor> all;
        loot::Each(all);
        const std::vector<int> grades = PileGrades(all);
        std::array<style::Rgba, 8> tiers;
        if (!loot::GradeColors(tiers))
            for (int g = 0; g < 8; g++) tiers[g] = style::rarity::Of(g);
        std::unordered_map<std::uintptr_t, Spark> keep;
        for (size_t i = 0; i < all.size(); i++) {
            if (grades[i] == kLooted) continue;
            void* a = reinterpret_cast<void*>(all[i].id);  // live: Each read it on this game-thread call
            auto it = g_sparks.find(all[i].id);
            if (it != g_sparks.end() && it->second.actor.Is(a) && fx::Alive(it->second.id)) {
                if (it->second.grade != grades[i]) Tint(it->second, grades[i], tiers);
                keep.emplace(it->first, it->second);
                g_sparks.erase(it);
                continue;
            }
            const bool chest = all[i].kind == loot::Kind::Chest;
            fx::Place p;
            p.z = chest ? kLiftChest : kLiftItem;
            p.scale = chest ? kScaleChest : kScaleItem;
            p.cull = kCull;
            Spark s{ref::Ref(a), fx::Attach(kOwner, kTemplate, a, p), -3};
            if (!s.id) continue;
            Tint(s, grades[i], tiers);
            keep.emplace(all[i].id, s);
            char b[120];
            std::snprintf(b, sizeof b, "[idle-loot] sparkle on %s %llx, grade %d", chest ? "chest" : "item", (unsigned long long)all[i].id, grades[i]);
            logger::log(b);
        }
        for (auto& [k, s] : g_sparks) {  // looted, streamed out or destroyed
            fx::Remove(s.id);
            logger::log("[idle-loot] sparkle off " + std::to_string(k));
        }
        g_sparks.swap(keep);
    }

    void OnLootEvent(void* obj, void*, void*) {  // a watched function ran: re-read loot state for kSettle
        if (game::OnGameThread() && !t_busy && loot::IsLootActor(obj)) g_settleUntil = Now() + kSettle;
    }

    void OnBeginPlay(void* obj, void* fn, void*) {
        if (!t_busy) loot::OnEvent(obj, fn);  // the tracker adds loot actors (Version() bumps)
    }

    void OnTick(void* obj, void* fn, void*) {
        if (t_busy || !game::OnGameThread()) return;
        loot::OnEvent(obj, fn);  // tracker: world scan on a new world, grade colours
        if (loot::Version() == g_version && Now() >= g_settleUntil) return;
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

        void Off() override {
            Listen(false);
            fx::Release(kOwner);  // the next world tick destroys our components
            g_on = false;
            g_sparks.clear();
            loot::Reset();
        }
    } g_idle_loot;
}

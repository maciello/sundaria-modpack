#include "xp-by-map-level.hpp"
#include "feature.hpp"
#include "game.hpp"
#include "logger.hpp"
#include "ref.hpp"
#include "umg.hpp"
#include "imgui.h"
#include "imgui_internal.h"  // MarkIniSettingsDirty

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <string_view>
#include <vector>
#include "Archon_classes.hpp"
#include "Archon_parameters.hpp"
#include "BP_GameModeDungeon_classes.hpp"
#include "BP_GameDifficultyComponent_classes.hpp"
#include "BP_ArchonGameFunctionLibrary_classes.hpp"
#include "BP_ArchonGameFunctionLibrary_parameters.hpp"

// XP by map level (#134). Host only. Kill XP is computed BP->BP inside the script VM (invisible to ProcessEvent), so
// a hero's gain is found as the rise of AArchonCharacter::GetExperience since our last read, checked after each
// OnDeathEvent (any Blueprint override: it is BlueprintImplementableEvent, so ProcessEvent sees the override class, never ArchonCharacter) and on the next world tick. The extra (m - 1) * gained is granted with the game's own
// GainExperience; our baseline is re-read after the grant, so it is never scaled again. m <= 1 grants nothing.
// Map level = AllRegions[DungeonSettings.RegionId].RecommendedLevels[DungeonSettings.Difficulty] (what the map screen
// shows), read once per dungeon game mode via the game's FindMapByWorldID. Facts: skill game-facts.md xp_by_map_level.
using namespace SDK;
using umg::PtrOk;

namespace {
    namespace xp = xp_by_map_level;

    // Game thread only.
    struct Hero { ref::Ref ch; int level = 0, exp = 0; };
    std::vector<Hero> g_heroes;
    ref::Ref g_gm;
    int g_mapLevel = 0, g_region = -1, g_diff = -1;

    std::atomic<bool> g_on{false}, g_recheck{false};
    std::atomic<float> g_k[6] = {5.0f, 0.05f, 55.0f, 15.0f, 20.0f, 6.0f};  // max min upC upW loC loW
    const char* const kKeys[6] = {"max", "min", "upC", "upW", "loC", "loW"};

    ref::Fn g_gain{AArchonCharacter::StaticClass, "ArchonCharacter", "GainExperience"};
    ref::Fn g_getExp{AArchonCharacter::StaticClass, "ArchonCharacter", "GetExperience"};
    ref::Fn g_getLvl{AArchonCharacter::StaticClass, "ArchonCharacter", "GetLevel"};
    ref::Fn g_findMap{UBP_ArchonGameFunctionLibrary_C::StaticClass, "BP_ArchonGameFunctionLibrary_C", "FindMapByWorldID"};

    xp::Curve CurveNow() {
        xp::Curve c;
        c.maxMult = g_k[0]; c.minMult = g_k[1]; c.upC = g_k[2]; c.upW = g_k[3]; c.loC = g_k[4]; c.loW = g_k[5];
        return c;
    }

    bool Read(AArchonCharacter* c, int& level, int& exp) {
        UFunction* fl = g_getLvl.Get();
        UFunction* fe = g_getExp.Get();
        if (!fl || !fe) return false;
        Params::ArchonCharacter_GetLevel pl{};
        Params::ArchonCharacter_GetExperience pe{};
        c->ProcessEvent(fl, &pl);
        c->ProcessEvent(fe, &pe);
        level = pl.ReturnValue; exp = pe.ReturnValue;
        return true;
    }

    ABP_GameModeDungeon_C* DungeonMode(UWorld* w) {
        AGameModeBase* gm = PtrOk(w) ? w->AuthorityGameMode : nullptr;
        return PtrOk(gm) && gm->IsA(ABP_GameModeDungeon_C::StaticClass()) ? static_cast<ABP_GameModeDungeon_C*>(gm) : nullptr;
    }

    // Once per game mode: region + tier -> map level. false = not ready yet (retry on the next event).
    bool ReadMap(UWorld* w, ABP_GameModeDungeon_C* gm) {
        UBP_GameDifficultyComponent_C* d = PtrOk(gm->BP_GameDifficultyComponent) ? gm->BP_GameDifficultyComponent : nullptr;
        UFunction* fn = g_findMap.Get();
        UBP_ArchonGameFunctionLibrary_C* lib = UBP_ArchonGameFunctionLibrary_C::GetDefaultObj();
        if (!d || !d->DungeonSettingsReady || !fn || !PtrOk(lib)) return false;
        g_region = d->DungeonSettings.RegionId_5_59B790AC4FC495440D646EB72EA42E8D;
        g_diff = int(d->DungeonSettings.Difficulty_2_2B61F47F4CB5FC26B5706C916FFCDF5E);
        Params::BP_ArchonGameFunctionLibrary_C_FindMapByWorldID p{};
        p.WorldID = g_region;
        p.__WorldContext = w;
        lib->ProcessEvent(fn, &p);
        auto& lv = p.MapData.RecommendedLevels_130_D86F517F4C28BB500B6E55AC7E6DD35C;
        g_mapLevel = (p.Found && g_diff >= 0 && g_diff < lv.Num()) ? lv[g_diff] : 0;
        g_gm = ref::Ref(gm);
        g_heroes.clear();
        char buf[160];
        std::snprintf(buf, sizeof buf, "[xp-map] map region %d tier %d -> level %d (found %d)", g_region, g_diff, g_mapLevel, int(p.Found));
        logger::log(buf);
        return true;
    }

    Hero* Find(AArchonCharacter* c) {
        for (Hero& h : g_heroes) if (h.ch.Is(c)) return &h;
        return nullptr;
    }

    void Check() {
        UWorld* w = UWorld::GetWorld();
        ABP_GameModeDungeon_C* gm = DungeonMode(w);
        if (!gm) return;
        if (!g_gm.Is(gm) && !ReadMap(w, gm)) return;
        const xp::Curve curve = CurveNow();
        for (int i = 0; i < gm->SpawnedPlayers.Num(); i++) {
            AController* pc = gm->SpawnedPlayers[i];
            APawn* pawn = PtrOk(pc) ? pc->Pawn : nullptr;
            if (!PtrOk(pawn) || !pawn->IsA(AArchonCharacter::StaticClass())) continue;
            auto* c = static_cast<AArchonCharacter*>(pawn);
            int lvl = 0, exp = 0;
            if (!Read(c, lvl, exp)) continue;
            Hero* h = Find(c);
            if (!h) { g_heroes.push_back({ref::Ref(c), lvl, exp}); continue; }
            const bool levelUp = lvl != h->level;
            const int gained = exp - h->exp;
            h->level = lvl; h->exp = exp;
            if (levelUp || gained <= 0) continue;  // level change: the rise cannot be told from the reset, rebased
            if (g_mapLevel <= 0) continue;
            const int d = g_mapLevel - lvl;
            const double m = xp::Mult(d, curve);
            const int extra = xp::Extra(gained, m);
            char buf[160];
            if (extra <= 0) {
                if (m < 1.0) { std::snprintf(buf, sizeof buf, "[xp-map] hero L%d map L%d d=%d x%.2f +0 (gained %d) skipped: below 1", lvl, g_mapLevel, d, m, gained); logger::log(buf); }
                continue;
            }
            UFunction* fg = g_gain.Get();
            if (!fg) continue;
            Params::ArchonCharacter_GainExperience gp{};
            gp.Exp = extra; gp.IsPVP = false;
            c->ProcessEvent(fg, &gp);
            std::snprintf(buf, sizeof buf, "[xp-map] hero L%d map L%d d=%d x%.2f +%d (gained %d)", lvl, g_mapLevel, d, m, extra, gained);
            logger::log(buf);
            Read(c, h->level, h->exp);  // our own grant is the new baseline
        }
    }

    void OnDeath(void*, void*, void*) {
        if (!g_on || !game::OnGameThread()) return;
        g_recheck = true;  // the grant may come after this event: the next world tick looks again
        Check();
    }

    void OnTick(void*, void*, void*) {
        if (!g_on || !game::OnGameThread()) return;
        UWorld* w = UWorld::GetWorld();
        ABP_GameModeDungeon_C* gm = DungeonMode(w);
        if (g_recheck.exchange(false) || (gm && !g_gm.Is(gm))) Check();
    }

    struct XpByMapLevel : feature::Feature {
        XpByMapLevel() : Feature("XP by map level", feature::Stage::Alpha) {}

        void OnFrame(const feature::Frame&) override {
            if (g_on.exchange(true)) return;
            game::On(nullptr, "OnDeathEvent", &OnDeath, true);
            game::OnWorldTick(&OnTick, true);
        }

        // Nothing in the world to restore; on=false returns once no callback is inside, so the state may be cleared.
        void Off() override {
            g_on = false;
            game::On(nullptr, "OnDeathEvent", &OnDeath, false);
            game::OnWorldTick(&OnTick, false);
            g_heroes.clear();
            g_gm = {};
        }

        void Menu() override {
            ImGui::TextDisabled("host only; dungeon: XP x curve(map level - hero level), only ever more XP");
            const struct { const char* label; float lo, hi; } s[6] = {
                {"max multiplier", 1.0f, 10.0f}, {"min multiplier (info: <1 grants nothing)", 0.0f, 1.0f},
                {"upper centre (levels)", 5.0f, 150.0f}, {"upper width", 1.0f, 60.0f},
                {"lower centre (levels)", 5.0f, 100.0f}, {"lower width", 1.0f, 40.0f}};
            for (int i = 0; i < 6; i++) {
                float v = g_k[i];
                if (ImGui::SliderFloat(s[i].label, &v, s[i].lo, s[i].hi, "%.2f")) { g_k[i] = v; ImGui::MarkIniSettingsDirty(); }
            }
        }

        void Load(const char* key, const char* value) override {
            for (int i = 0; i < 6; i++) if (std::string_view(key) == kKeys[i]) g_k[i] = std::strtof(value, nullptr);
        }
        void Save(std::vector<std::pair<std::string, std::string>>& out) override {
            for (int i = 0; i < 6; i++) out.emplace_back(kKeys[i], std::to_string(g_k[i].load()));
        }
    } g_xp_by_map_level;
}

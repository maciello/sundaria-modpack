#include "feature.hpp"
#include "training-ally.hpp"
#include "draw.hpp"
#include "drain.hpp"
#include "logger.hpp"
#include "ref.hpp"
#include "style.hpp"
#include "umg.hpp"
#include "imgui.h"
#include "imgui_internal.h"  // MarkIniSettingsDirty

#include <Windows.h>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>
#include "Engine_classes.hpp"
#include "Archon_classes.hpp"
#include "GameplayAbilities_classes.hpp"
#include "BP_CharacterBase_classes.hpp"
#include "BP_CharacterBase_parameters.hpp"

// Training ally (#125, dev): one training dummy counts as your ally, so heals and buffs can be tested alone. The game
// decides friend or enemy by kingdom id (game-facts.md § targeting): the adopted character gets the hero's id through
// the game's own I_SetKingdomId. Its spot is remembered per map, so it is adopted again whenever that map loads.
// Above its head: the stats buffs move and every active effect; a value that changed since adoption shows the change.
// Host only. Off / Release gives the kingdom back. Game thread, except the drawing (render thread, plain data).
using namespace SDK;

namespace {
    bool PtrOk(const void* p) {
        const uintptr_t v = reinterpret_cast<uintptr_t>(p);
        return v > 0x10000 && v < 0x7FFFFFFFFFFFull;
    }

    ref::Fn g_fnSetKingdom{ABP_CharacterBase_C::StaticClass, "BP_CharacterBase_C", "I_SetKingdomId"};

    // Remembered spot (dos-tool.ini), written by the game thread on adopt, read by Save on the render thread.
    SRWLOCK g_mu = SRWLOCK_INIT;
    std::string g_map;
    float g_spot[3]{};
    bool g_haveSpot = false, g_dirty = false;

    std::atomic<bool> g_on{false}, g_wantAdopt{false}, g_wantRelease{false};

    // Game-thread state.
    ref::Ref g_ally, g_world;
    int g_origKingdom = 0;
    float g_base[training_ally::kRowCount]{};
    ULONGLONG g_nextStats = 0, g_nextSearch = 0, g_searchUntil = 0;
    thread_local bool t_busy = false;

    // What the render thread draws.
    struct Shown {
        bool on = false;
        float head[3]{};
        std::vector<std::pair<std::string, bool>> lines;  // text, changed
        std::vector<std::string> effects;
    };
    Shown g_shown;
    std::string g_status;  // menu line
    game::Drain g_drain;   // Off(): the game thread gives the kingdom back

    void Publish(Shown s) {
        AcquireSRWLockExclusive(&g_mu);
        g_shown = std::move(s);
        ReleaseSRWLockExclusive(&g_mu);
    }
    void Status(const std::string& s) {
        AcquireSRWLockExclusive(&g_mu);
        g_status = s;
        ReleaseSRWLockExclusive(&g_mu);
        logger::log(("[training-ally] " + s).c_str());
    }

    AArchonCharacter* Hero() {
        APlayerController* pc = umg::LocalPC();
        return PtrOk(pc) && PtrOk(pc->Pawn) && pc->Pawn->IsA(AArchonCharacter::StaticClass()) ? static_cast<AArchonCharacter*>(pc->Pawn) : nullptr;
    }

    void SetKingdom(AArchonCharacter* c, int id) {
        UFunction* fn = g_fnSetKingdom.Get();
        if (fn && c->IsA(ABP_CharacterBase_C::StaticClass())) {
            Params::BP_CharacterBase_C_I_SetKingdomId p{};
            p.OwningKingdomId = id;
            c->ProcessEvent(fn, &p);
        }
        if (c->ReplicatedKingdomId != id) c->ReplicatedKingdomId = id;  // I_GetKingdomId returns this field
    }

    bool ReadStats(AArchonCharacter* c, float out[training_ally::kRowCount]) {
        using namespace training_ally;
        UAbilitySystemComponent* asc = c->mAbilitySystemComponent;
        if (!PtrOk(asc)) return false;
        std::fill(out, out + kRowCount, 0.0f);
        bool any = false;
        for (int i = 0; i < asc->SpawnedAttributes.Num(); i++) {
            UAttributeSet* set = asc->SpawnedAttributes[i];
            if (!PtrOk(set)) continue;
            if (set->IsA(UArchonAttributeSet_Status::StaticClass())) {
                out[Health] = static_cast<UArchonAttributeSet_Status*>(set)->CurrentHealth;
                any = true;
            } else if (set->IsA(UArchonAttributeSet_Secondary::StaticClass())) {
                auto* s = static_cast<UArchonAttributeSet_Secondary*>(set);
                out[Melee] = s->Map, out[Ranged] = s->RAP, out[Spell] = s->SP;
                out[Crit] = s->CriticalChance;
                out[CritMelee] = s->CriticalDamage_Melee, out[CritRanged] = s->CriticalDamage_Range, out[CritSpell] = s->CriticalDamage_Spell;
                out[AttackSpeed] = s->IncreaseAttackSpeed, out[MoveSpeed] = s->IncreaseMovementSpeed;
            } else if (set->IsA(UArchonAbilitySystemAttributeSet::StaticClass())) {
                auto* s = static_cast<UArchonAbilitySystemAttributeSet*>(set);
                out[DamageOut] = s->OutgoingDamageMod, out[DamageIn] = s->IncomingDamageMod;
                out[CritOut] = s->OutgoingCritDamageMod, out[Cooldown] = s->CooldownDurationMultiplier;
            }
        }
        return any;
    }

    void Adopt(AArchonCharacter* c, AArchonCharacter* hero, bool remember) {
        g_origKingdom = c->ReplicatedKingdomId;
        SetKingdom(c, hero->ReplicatedKingdomId);
        g_ally = ref::Ref(c);
        ReadStats(c, g_base);
        g_nextStats = 0;
        const FVector at = c->K2_GetActorLocation();
        if (remember) {
            AcquireSRWLockExclusive(&g_mu);
            g_map = UWorld::GetWorld()->GetName();
            g_spot[0] = at.X, g_spot[1] = at.Y, g_spot[2] = at.Z;
            g_haveSpot = g_dirty = true;
            ReleaseSRWLockExclusive(&g_mu);
        }
        char buf[200];
        std::snprintf(buf, sizeof(buf), "adopted %s at %.0f %.0f %.0f (kingdom %d -> %d)", c->Class->GetName().c_str(), at.X, at.Y, at.Z,
                      g_origKingdom, hero->ReplicatedKingdomId);
        Status(buf);
    }

    void Release(bool forget) {
        if (auto* c = g_ally.Get<AArchonCharacter>()) SetKingdom(c, g_origKingdom);
        g_ally = {};
        Publish({});
        if (forget) {
            AcquireSRWLockExclusive(&g_mu);
            g_haveSpot = false;
            g_dirty = true;
            ReleaseSRWLockExclusive(&g_mu);
            Status("released");
        }
    }

    // The character under the crosshair: a sphere sweep along the camera (pawns only).
    AArchonCharacter* UnderCrosshair(APlayerController* pc, AArchonCharacter* hero) {
        if (!PtrOk(pc->PlayerCameraManager)) return nullptr;
        const FVector cam = pc->PlayerCameraManager->GetCameraLocation();
        const FRotator r = pc->PlayerCameraManager->GetCameraRotation();
        const float p = r.Pitch * 3.14159265f / 180.0f, y = r.Yaw * 3.14159265f / 180.0f;
        const FVector end{cam.X + std::cos(p) * std::cos(y) * 5000.0f, cam.Y + std::cos(p) * std::sin(y) * 5000.0f, cam.Z + std::sin(p) * 5000.0f};
        AActor* ignoreArr[1] = {hero};
        const TArray<AActor*> ignored(ignoreArr, 1, 1);
        EObjectTypeQuery typesArr[1] = {EObjectTypeQuery::ObjectTypeQuery3};  // Pawn
        const TArray<EObjectTypeQuery> types(typesArr, 1, 1);
        FHitResult hit{};
        if (!UKismetSystemLibrary::SphereTraceSingleForObjects(hero, cam, end, 40.0f, types, false, ignored, EDrawDebugTrace::None, &hit, true,
                                                               FLinearColor{}, FLinearColor{}, 0.0f))
            return nullptr;
        AActor* a = hit.Actor.Get();
        return PtrOk(a) && a->IsA(AArchonCharacter::StaticClass()) ? static_cast<AArchonCharacter*>(a) : nullptr;
    }

    // The remembered dummy: the character closest to its spot. One overlap query, at most once a second while searching.
    AArchonCharacter* AtSpot(AArchonCharacter* hero, const float spot[3]) {
        AActor* ignoreArr[1] = {hero};
        const TArray<AActor*> ignored(ignoreArr, 1, 1);
        EObjectTypeQuery typesArr[1] = {EObjectTypeQuery::ObjectTypeQuery3};
        const TArray<EObjectTypeQuery> types(typesArr, 1, 1);
        TArray<AActor*> found;  // ponytail: the engine allocates this array and Dumper-7's TArray never frees it (a few bytes per search)
        UKismetSystemLibrary::SphereOverlapActors(hero, FVector{spot[0], spot[1], spot[2]}, 250.0f, types, AArchonCharacter::StaticClass(), ignored, &found);
        AArchonCharacter* best = nullptr;
        float bestD = 1e9f;
        for (int i = 0; i < found.Num(); i++) {
            AActor* a = found[i];
            if (!PtrOk(a) || !a->IsA(AArchonCharacter::StaticClass())) continue;
            const FVector at = a->K2_GetActorLocation();
            if (training_ally::Nearer(at.X - spot[0], at.Y - spot[1], at.Z - spot[2], 250.0f, bestD)) best = static_cast<AArchonCharacter*>(a);
        }
        return best;
    }

    void Show(AArchonCharacter* c) {
        using namespace training_ally;
        Shown s;
        s.on = true;
        const FVector at = c->K2_GetActorLocation();
        const float half = PtrOk(c->CapsuleComponent) ? c->CapsuleComponent->GetScaledCapsuleHalfHeight() : 90.0f;
        s.head[0] = at.X, s.head[1] = at.Y, s.head[2] = at.Z + half;
        float now[kRowCount];
        if (ReadStats(c, now))
            for (int i = 0; i < kRowCount; i++) {
                const bool changed = Changed(g_base[i], now[i]);
                if (changed || now[i] != 0 || i == Health) s.lines.push_back({Line(kRows[i], g_base[i], now[i]), changed});  // zero and untouched: not shown
            }
        if (UAbilitySystemComponent* asc = c->mAbilitySystemComponent; PtrOk(asc)) {
            const float t = UGameplayStatics::GetTimeSeconds(c);
            const auto& fx = asc->ActiveGameplayEffects.GameplayEffects_Internal;
            for (int i = 0; i < fx.Num() && s.effects.size() < 8; i++) {
                const FActiveGameplayEffect& e = fx[i];
                if (!PtrOk(e.Spec.Def) || !PtrOk(e.Spec.Def->Class)) continue;
                s.effects.push_back(Effect(e.Spec.Def->Class->GetName(), e.Spec.Duration > 0 ? e.Spec.Duration - (t - e.StartWorldTime) : -1.0f));
            }
        }
        Publish(std::move(s));
    }

    void OnTick(void*, void*, void*) {
        if (t_busy || !g_on.load(std::memory_order_relaxed) || !game::OnGameThread()) return;
        APlayerController* pc = umg::LocalPC();
        AArchonCharacter* hero = Hero();
        if (!hero) return;
        t_busy = true;
        const ULONGLONG now = GetTickCount64();
        UWorld* w = UWorld::GetWorld();
        if (!g_world.Is(w)) {  // a new map: the old ally is gone; look for the remembered one for a while (it spawns late)
            g_world = ref::Ref(w);
            g_ally = {};
            Publish({});
            g_searchUntil = now + 60000;
            g_nextSearch = 0;
        }
        if (g_wantRelease.exchange(false)) Release(true);
        if (g_wantAdopt.exchange(false)) {
            if (AArchonCharacter* c = UnderCrosshair(pc, hero)) {
                if (g_ally.Get<AArchonCharacter>()) Release(false);
                Adopt(c, hero, true);
            } else {
                Status("no character under the crosshair");
            }
        }
        AArchonCharacter* ally = g_ally.Get<AArchonCharacter>();
        if (!ally && now < g_searchUntil && now >= g_nextSearch) {
            g_nextSearch = now + 1000;
            AcquireSRWLockShared(&g_mu);
            const bool here = g_haveSpot && g_map == w->GetName();
            const float spot[3] = {g_spot[0], g_spot[1], g_spot[2]};
            ReleaseSRWLockShared(&g_mu);
            if (!here) g_searchUntil = 0;
            else if (AArchonCharacter* c = AtSpot(hero, spot)) { Adopt(c, hero, false); ally = c; g_searchUntil = 0; }
        }
        if (ally && now >= g_nextStats) {
            g_nextStats = now + 200;
            if (ally->ReplicatedKingdomId != hero->ReplicatedKingdomId) SetKingdom(ally, hero->ReplicatedKingdomId);  // the game reset it
            Show(ally);
        }
        t_busy = false;
    }

    struct TrainingAlly : feature::Feature {
        bool listening = false;
        TrainingAlly() : Feature("Training ally", feature::Stage::Alpha) {}

        void OnFrame(const feature::Frame& f) override {
            if (!listening) { listening = true; g_on = true; game::OnWorldTick(&OnTick, true); }
            AcquireSRWLockShared(&g_mu);
            const Shown s = g_shown;
            const bool dirty = g_dirty;
            ReleaseSRWLockShared(&g_mu);
            if (dirty) { g_dirty = false; ImGui::MarkIniSettingsDirty(); }
            combat::View view{};
            float sx, sy;
            if (!s.on || !game::GetView(view) || !combat::Project(view, s.head[0], s.head[1], s.head[2], f.w, f.h, sx, sy)) return;
            const float dx = s.head[0] - view.x, dy = s.head[1] - view.y, dz = s.head[2] - view.z;
            const float dist = std::sqrt(dx * dx + dy * dy + dz * dz);
            if (dist > 4000.0f) return;
            const float depth = std::clamp(1500.0f / std::max(dist, 1.0f), 0.6f, 1.25f), ui = style::type::Ui(f.h);
            const float ts = std::max(style::type::kXs, style::type::kSm * ui * depth), lh = ts * 1.15f;
            ImDrawList* dl = ImGui::GetBackgroundDrawList();
            const ImU32 ink = style::Pack(style::color::kInk, style::stroke::kOutlineAlpha);
            float y = sy - (float(s.lines.size() + s.effects.size()) + 1.6f) * lh;  // the block ends just above the head
            auto text = [&](const std::string& t, style::Rgba col) {
                const ImVec2 sz = f.font->CalcTextSizeA(ts, FLT_MAX, 0, t.c_str());
                draw::OutlinedText(dl, f.font, ts, {sx - sz.x * 0.5f, y}, style::Pack(col, 1.0f), ink, style::stroke::Outline(ts), t.c_str());
                y += lh;
            };
            text("TRAINING ALLY", style::color::kHeal);
            for (const auto& [line, changed] : s.lines) text(line, changed ? style::color::kGood : style::color::kTextSoft);
            for (const std::string& e : s.effects) text(e, style::color::kText);
        }

        void Off() override {
            g_wantRelease = false;
            g_wantAdopt = false;
            g_on = false;
            if (listening) game::OnWorldTick(&OnTick, false);
            listening = false;
            // Off() is the render thread: the kingdom goes back on the game thread (I_SetKingdomId), which also forgets
            // the ally there (the drain owns g_ally / g_world from here).
            g_drain.Request(true, "training-ally", [] { Release(false); g_world = {}; });
        }

        void Menu() override {
            if (ImGui::Button("Adopt the character I look at")) g_wantAdopt = true;
            ImGui::SameLine();
            if (ImGui::Button("Release")) g_wantRelease = true;
            AcquireSRWLockShared(&g_mu);
            const std::string st = g_status;
            const bool have = g_haveSpot;
            const std::string map = g_map;
            ReleaseSRWLockShared(&g_mu);
            if (have) ImGui::TextDisabled("remembered in %s; adopted again whenever that map loads", map.c_str());
            ImGui::TextDisabled("%s", st.empty() ? "counts as your ally: heals and buffs can target it (host only)" : st.c_str());
        }

        void Load(const char* key, const char* value) override {
            const std::string k = key;
            AcquireSRWLockExclusive(&g_mu);
            if (k == "map") { g_map = value; g_haveSpot = !g_map.empty(); }
            else if (k == "x") g_spot[0] = float(std::atof(value));
            else if (k == "y") g_spot[1] = float(std::atof(value));
            else if (k == "z") g_spot[2] = float(std::atof(value));
            ReleaseSRWLockExclusive(&g_mu);
        }
        void Save(std::vector<std::pair<std::string, std::string>>& out) override {
            AcquireSRWLockShared(&g_mu);
            out.push_back({"map", g_haveSpot ? g_map : ""});
            out.push_back({"x", std::to_string(g_spot[0])});
            out.push_back({"y", std::to_string(g_spot[1])});
            out.push_back({"z", std::to_string(g_spot[2])});
            ReleaseSRWLockShared(&g_mu);
        }
    } g_trainingAlly;
}

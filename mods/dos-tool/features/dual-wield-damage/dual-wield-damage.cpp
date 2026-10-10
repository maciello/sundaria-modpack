#include "dual-wield-damage.hpp"
#include "feature.hpp"
#include "game.hpp"
#include "logger.hpp"
#include "ref.hpp"
#include "drain.hpp"
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
#include "BP_CharacterBase_classes.hpp"
#include "BP_AffixContainerEquip_classes.hpp"
#include "BP_AffixContainerEquip_parameters.hpp"
#include "BP_GameAbilityBase_classes.hpp"
#include "BP_ProjectileBase_classes.hpp"
#include "BP_GameAbility_MeleeAttack_classes.hpp"
#include "BP_GameAbility_Eviscerate_classes.hpp"
#include "BP_GameAbility_ShootArrow_classes.hpp"
#include "BP_GameAbility_AimedShot_classes.hpp"
#include "BP_GameAbility_Smite_classes.hpp"
#include "BP_GameAbility_FireBall_classes.hpp"

// Dual-wield damage (#136): while a character holds a weapon in both hands, each hand's weapon WeaponDamage becomes
// own + k * other, so a cast (one hand per cast, alternating) deals both. Facts: skill game-facts.md
// dps_mechanics.weapon_damage_hands. The script VM reads UArchonAttributeSet_Secondary::WeaponDamage (a plain float)
// of the item's cached attribute set (BP_ItemContainerComponent::ItemAttributeSet, rebuilt from the item struct, not
// saved); ReCalculateEquippedItemAttributeSet recreates every equipped set (Recalculate=true) and THEN broadcasts
// FOnEquipContainerAttributeSetUpdate, so a listener on that event always starts from fresh vanilla values.
// Primary ability (the class's basic attack: MeleeAttack, Eviscerate, ShootArrow, AimedShot, Smite, FireBall) keeps
// vanilla: damage is applied inside the ability's GameplayAnimNotifyEvent / OnProjectileHit (both FUNC_Event, native
// code enters them through ProcessEvent with obj = the ability instance, whose Outer is the avatar character), so a
// pre-call event filter writes the captured vanilla values and the post-call listener writes the boosted ones back.
// Host only: the event fires under HasAuthority. A remote character is handled on its next equip event.
using namespace SDK;
using umg::PtrOk;

namespace {
    namespace dw = dual_wield_damage;

    // Game thread only (listener, world tick, Drain).
    struct Rec { ref::Ref ch, setL, setR; float vl = 0, vr = 0, bl = 0, br = 0; };
    std::vector<Rec> g_recs;
    std::atomic<bool> g_on{false}, g_dirty{false};
    std::atomic<int> g_has{0};                       // g_recs.size(), readable from the filter on any thread
    std::atomic<int32_t> g_nNotify{-1}, g_nHit{-1}, g_nRecv{-1};  // FName indices of the damage entry events, seeded on the world tick
    ref::Ref g_swapped;                              // character whose weapons currently hold vanilla for a primary hit
    int g_swapLogs = 0;
    std::atomic<float> g_k{dw::kDefault};
    game::Drain g_drain;
    ref::Fn g_notifyFn{UBP_GameAbilityBase_C::StaticClass, "BP_GameAbilityBase_C", "GameplayAnimNotifyEvent"};
    ref::Fn g_hitFn{UBP_GameAbilityBase_C::StaticClass, "BP_GameAbilityBase_C", "OnProjectileHit"};
    ref::Fn g_recvFn{ABP_ProjectileBase_C::StaticClass, "BP_ProjectileBase_C", "ReceiveHit"};
    ref::Cached<UClass> g_projCls{[] { return ABP_ProjectileBase_C::StaticClass(); }};
    ref::Cached<UClass> g_primary[] = {
        {[] { return UBP_GameAbility_MeleeAttack_C::StaticClass(); }}, {[] { return UBP_GameAbility_Eviscerate_C::StaticClass(); }},
        {[] { return UBP_GameAbility_ShootArrow_C::StaticClass(); }},  {[] { return UBP_GameAbility_AimedShot_C::StaticClass(); }},
        {[] { return UBP_GameAbility_Smite_C::StaticClass(); }},       {[] { return UBP_GameAbility_FireBall_C::StaticClass(); }}};
    ref::Fn g_hand{UBP_AffixContainerEquip_C::StaticClass, "BP_AffixContainerEquip_C", "I_GetActiveWeaponAttributeSet"};

    UArchonAttributeSet_Secondary* Set(const ref::Ref& r) { return r.Get<UArchonAttributeSet_Secondary>(); }

    void Restore(Rec& r) {
        if (auto* s = Set(r.setL)) s->WeaponDamage = r.vl;
        if (auto* s = Set(r.setR)) s->WeaponDamage = r.vr;
        r.setL = r.setR = {};
    }

    UArchonAttributeSet_Secondary* Hand(UBP_AffixContainerEquip_C* eq, bool left) {
        UFunction* fn = g_hand.Get();
        if (!fn) return nullptr;
        Params::BP_AffixContainerEquip_C_I_GetActiveWeaponAttributeSet p{};
        p.isLeftHand = left;
        eq->ProcessEvent(fn, &p);
        return PtrOk(p.AttributeSet) ? p.AttributeSet : nullptr;
    }

    // Restore what we wrote on this character, then (re)apply from its current vanilla values.
    void Apply(ABP_CharacterBase_C* c) {
        Rec* rec = nullptr;
        for (Rec& r : g_recs) if (r.ch.Is(c)) rec = &r;
        if (rec) Restore(*rec);
        UBP_AffixContainerEquip_C* eq = PtrOk(c->ItemContainerEquip) ? c->ItemContainerEquip : nullptr;
        UArchonAttributeSet_Secondary* l = eq ? Hand(eq, true) : nullptr;
        UArchonAttributeSet_Secondary* r = eq ? Hand(eq, false) : nullptr;
        const dw::Out o = dw::Plan({l != nullptr, r != nullptr, l == r, l ? l->WeaponDamage : 0.0f, r ? r->WeaponDamage : 0.0f}, g_k);
        if (!o.apply) {
            if (rec) { logger::log("[dual-wield] off for " + c->GetName()); g_recs.erase(g_recs.begin() + (rec - g_recs.data())); g_has = int(g_recs.size()); }
            return;
        }
        if (!rec) { g_recs.emplace_back(); rec = &g_recs.back(); rec->ch = ref::Ref(c); }
        rec->setL = ref::Ref(l); rec->setR = ref::Ref(r);
        rec->vl = l->WeaponDamage; rec->vr = r->WeaponDamage;
        char buf[160];
        std::snprintf(buf, sizeof buf, "[dual-wield] %s L %.1f->%.1f R %.1f->%.1f (k %.2f)", c->GetName().c_str(), rec->vl, o.l, rec->vr, o.r, double(g_k));
        logger::log(buf);
        rec->bl = o.l; rec->br = o.r;
        l->WeaponDamage = o.l;
        r->WeaponDamage = o.r;
        g_has = int(g_recs.size());
    }

    bool IsPlayer(UObject* o) {
        return PtrOk(o) && o->IsA(ABP_CharacterBase_C::StaticClass()) && PtrOk(static_cast<APawn*>(o)->PlayerState);
    }

    void OnEquip(void* obj, void*, void*) {
        if (!g_on || !game::OnGameThread() || !IsPlayer(static_cast<UObject*>(obj))) return;
        Apply(static_cast<ABP_CharacterBase_C*>(obj));
    }

    Rec* Find(const ref::Ref& ch) {
        for (Rec& r : g_recs) if (r.ch == ch) return &r;
        return nullptr;
    }

    // Before the game's call (any thread: O(1) rejects first). A primary ability's hit on a dual-wielder reads vanilla.
    bool Filter(void* obj, void* fn, void*) {
        if (!g_has.load(std::memory_order_relaxed) || !fn) return false;
        const int32_t n = static_cast<UFunction*>(fn)->Name.ComparisonIndex;
        const bool recv = n == g_nRecv.load(std::memory_order_relaxed);
        if (n != g_nNotify.load(std::memory_order_relaxed) && n != g_nHit.load(std::memory_order_relaxed) && !recv) return false;
        auto* o = static_cast<UObject*>(obj);
        if (!game::OnGameThread() || !g_on || !PtrOk(o) || !PtrOk(o->Class) || g_swapped.ptr) return false;
        // A projectile's hit (BP_ProjectileBase::ReceiveHit -> ServerApplyGameplayEffectsOnExplode) calls the ability's
        // OnProjectileHit inside the script VM, so the damage runs inside ReceiveHit with obj = the projectile.
        if (recv) {
            UClass* pc = g_projCls.Get();
            if (!pc || !o->IsA(pc)) return false;
            o = static_cast<ABP_ProjectileBase_C*>(o)->InstigatorAbility;
            if (!PtrOk(o) || !PtrOk(o->Class)) return false;
        }
        bool primary = false;
        for (auto& c : g_primary) primary |= o->Class == c.Get();
        if (!primary) return false;
        Rec* rec = PtrOk(o->Outer) ? Find(ref::Ref(o->Outer)) : nullptr;
        if (!rec) {
            static bool once = false;
            if (!once) { once = true; logger::log("[dual-wield] primary hit: ability Outer is no tracked character: " + o->Class->GetName()); }
            return false;
        }
        if (auto* s = Set(rec->setL)) s->WeaponDamage = rec->vl;
        if (auto* s = Set(rec->setR)) s->WeaponDamage = rec->vr;
        g_swapped = rec->ch;
        if (g_swapLogs++ < 8) logger::log("[dual-wield] primary " + o->Class->GetName() + ": vanilla for this hit");
        return false;
    }

    // After the call: boosted again.
    void AfterHit(void*, void*, void*) {
        if (!g_swapped.ptr || !game::OnGameThread()) return;
        const ref::Ref ch = g_swapped;
        g_swapped = {};
        if (Rec* rec = Find(ch)) {
            if (auto* s = Set(rec->setL)) s->WeaponDamage = rec->bl;
            if (auto* s = Set(rec->setR)) s->WeaponDamage = rec->br;
        }
    }

    // World tick: k changed / just enabled -> redo the known characters and the local one.
    void OnTick(void*, void*, void*) {
        if (!g_on || !game::OnGameThread()) return;
        if (g_nRecv < 0) {
            UFunction* a = g_notifyFn.Get(); UFunction* b = g_hitFn.Get(); UFunction* c = g_recvFn.Get();
            if (PtrOk(a) && PtrOk(b) && PtrOk(c)) {
                g_nNotify = a->Name.ComparisonIndex; g_nHit = b->Name.ComparisonIndex; g_nRecv = c->Name.ComparisonIndex;
            }
        }
        if (!g_dirty.exchange(false)) return;
        std::vector<ref::Ref> known;
        for (const Rec& r : g_recs) known.push_back(r.ch);
        for (const ref::Ref& r : known) if (auto* c = r.Get<ABP_CharacterBase_C>()) Apply(c);
        UWorld* w = UWorld::GetWorld();
        if (!PtrOk(w) || !PtrOk(w->OwningGameInstance) || w->OwningGameInstance->LocalPlayers.Num() <= 0) return;
        ULocalPlayer* lp = w->OwningGameInstance->LocalPlayers[0];
        APlayerController* pc = PtrOk(lp) ? lp->PlayerController : nullptr;
        APawn* pawn = PtrOk(pc) ? pc->Pawn : nullptr;
        if (IsPlayer(pawn)) Apply(static_cast<ABP_CharacterBase_C*>(pawn));
    }

    void RestoreAll() {
        for (Rec& r : g_recs) Restore(r);
        g_recs.clear();
        g_has = 0;
        g_swapped = {};
        logger::log("[dual-wield] restored vanilla");
    }

    struct DualWieldDamage : feature::Feature {
        DualWieldDamage() : Feature("Dual-wield damage", feature::Stage::Alpha) {}

        void OnFrame(const feature::Frame&) override {
            if (g_on.exchange(true)) return;
            game::On("BP_CharacterBase_C", "FOnEquipContainerAttributeSetUpdate", &OnEquip, true);
            game::On(nullptr, "GameplayAnimNotifyEvent", &AfterHit, true);
            game::On(nullptr, "OnProjectileHit", &AfterHit, true);
            game::On(nullptr, "ReceiveHit", &AfterHit, true);
            game::SetEventFilter(&Filter, true);
            game::OnWorldTick(&OnTick, true);
            g_dirty = true;
        }

        // Render thread: unsubscribe, hand the restore to the game thread.
        void Off() override {
            g_on = false;
            game::On("BP_CharacterBase_C", "FOnEquipContainerAttributeSetUpdate", &OnEquip, false);
            game::OnWorldTick(&OnTick, false);
            game::SetEventFilter(&Filter, false);
            game::On(nullptr, "GameplayAnimNotifyEvent", &AfterHit, false);
            game::On(nullptr, "OnProjectileHit", &AfterHit, false);
            game::On(nullptr, "ReceiveHit", &AfterHit, false);
            g_drain.Request(true, "dual-wield", [] { RestoreAll(); });
        }

        void Menu() override {
            float k = g_k;
            ImGui::TextDisabled("host only; two weapons: each cast = own + k x other hand");
            if (ImGui::SliderFloat("k (other hand)", &k, 0.0f, dw::kMax, "%.2f")) {
                g_k = k;
                g_dirty = true;
                ImGui::MarkIniSettingsDirty();
            }
        }

        void Load(const char* key, const char* value) override {
            if (std::string_view(key) == "k") g_k = std::strtof(value, nullptr);
        }
        void Save(std::vector<std::pair<std::string, std::string>>& out) override {
            out.emplace_back("k", std::to_string(g_k.load()));
        }
    } g_feature;
}

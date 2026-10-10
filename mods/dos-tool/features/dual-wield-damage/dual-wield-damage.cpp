#include "dual-wield-damage.hpp"
#include "feature.hpp"
#include "game.hpp"
#include "logger.hpp"
#include "ref.hpp"
#include "drain.hpp"
#include "umg.hpp"
#include "imgui.h"

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

// Dual-wield damage (#136): while a character holds a weapon in both hands, each hand's weapon WeaponDamage becomes
// own + k * other, so a cast (one hand per cast, alternating) deals both. Facts: skill game-facts.md
// dps_mechanics.weapon_damage_hands. The script VM reads UArchonAttributeSet_Secondary::WeaponDamage (a plain float)
// of the item's cached attribute set (BP_ItemContainerComponent::ItemAttributeSet, rebuilt from the item struct, not
// saved); ReCalculateEquippedItemAttributeSet recreates every equipped set (Recalculate=true) and THEN broadcasts
// FOnEquipContainerAttributeSetUpdate, so a listener on that event always starts from fresh vanilla values.
// Host only: the event fires under HasAuthority. A remote character is handled on its next equip event.
using namespace SDK;
using umg::PtrOk;

namespace {
    namespace dw = dual_wield_damage;

    // Game thread only (listener, world tick, Drain).
    struct Rec { ref::Ref ch, setL, setR; float vl = 0, vr = 0; };
    std::vector<Rec> g_recs;
    std::atomic<bool> g_on{false}, g_dirty{false};
    std::atomic<float> g_k{dw::kDefault};
    game::Drain g_drain;
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
            if (rec) { logger::log("[dual-wield] off for " + c->GetName()); g_recs.erase(g_recs.begin() + (rec - g_recs.data())); }
            return;
        }
        if (!rec) { g_recs.emplace_back(); rec = &g_recs.back(); rec->ch = ref::Ref(c); }
        rec->setL = ref::Ref(l); rec->setR = ref::Ref(r);
        rec->vl = l->WeaponDamage; rec->vr = r->WeaponDamage;
        char buf[160];
        std::snprintf(buf, sizeof buf, "[dual-wield] %s L %.1f->%.1f R %.1f->%.1f (k %.2f)", c->GetName().c_str(), rec->vl, o.l, rec->vr, o.r, double(g_k));
        logger::log(buf);
        l->WeaponDamage = o.l;
        r->WeaponDamage = o.r;
    }

    bool IsPlayer(UObject* o) {
        return PtrOk(o) && o->IsA(ABP_CharacterBase_C::StaticClass()) && PtrOk(static_cast<APawn*>(o)->PlayerState);
    }

    void OnEquip(void* obj, void*, void*) {
        if (!g_on || !game::OnGameThread() || !IsPlayer(static_cast<UObject*>(obj))) return;
        Apply(static_cast<ABP_CharacterBase_C*>(obj));
    }

    // World tick: k changed / just enabled -> redo the known characters and the local one.
    void OnTick(void*, void*, void*) {
        if (!g_on || !game::OnGameThread() || !g_dirty.exchange(false)) return;
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
        logger::log("[dual-wield] restored vanilla");
    }

    struct DualWieldDamage : feature::Feature {
        DualWieldDamage() : Feature("Dual-wield damage", feature::Stage::Alpha) {}

        void OnFrame(const feature::Frame&) override {
            if (g_on.exchange(true)) return;
            game::On("BP_CharacterBase_C", "FOnEquipContainerAttributeSetUpdate", &OnEquip, true);
            game::OnWorldTick(&OnTick, true);
            g_dirty = true;
        }

        // Render thread: unsubscribe, hand the restore to the game thread.
        void Off() override {
            g_on = false;
            game::On("BP_CharacterBase_C", "FOnEquipContainerAttributeSetUpdate", &OnEquip, false);
            game::OnWorldTick(&OnTick, false);
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

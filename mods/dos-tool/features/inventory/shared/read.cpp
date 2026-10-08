#include "sdk.hpp"
#include "logger.hpp"
#include <Windows.h>
#include "BP_InvManagerComponent_classes.hpp"
#include "BP_ItemContainerComponent_parameters.hpp"
#include "BP_ItemContainerStorage_classes.hpp"
#include "BP_SpecItemWeapon_classes.hpp"
#include "BP_SpecItemArmor_classes.hpp"
#include "BP_SpecItemCommon_classes.hpp"

// Item reads for every item feature (#16). Game thread. Facts: references/game-facts.md § items.
using namespace items;
using namespace items::sdk;
using namespace items::io;

namespace {
    std::string Text(const FText& t, const std::string& fallback) {
        const std::string s = PtrOk(t.TextData) ? t.ToString() : "";
        return s.empty() ? fallback : s;
    }
    std::string At(const std::vector<std::string>& v, int i) { return i >= 0 && i < int(v.size()) ? v[i] : ""; }

    SRWLOCK g_mu = SRWLOCK_INIT;  // not std::mutex (gotchas). Guards g_bankCache.
    std::vector<Item> g_bankCache;
    bool g_bankSeen = false;

    UArchonAttributeSet_Secondary* AttrSet(UBP_ItemContainerComponent_C* c, const Item& it) {
        static UFunction* fn = nullptr;
        if (!fn) fn = c->Class->GetFunction("BP_ItemContainerComponent_C", "GetItemAttributeSet");
        if (!fn) return nullptr;
        Params::BP_ItemContainerComponent_C_GetItemAttributeSet p{};
        p.ItemSlot = it.slot;
        p.ContainerType = EItemContainerType(it.containerType);
        p.ForceRecalculate = false;
        c->ProcessEvent(fn, &p);
        return PtrOk(p.SecondaryAttributeSet) ? p.SecondaryAttributeSet : nullptr;
    }
}

namespace items::sdk {
    ABP_PlayerControllerOnline_C* LocalPC() {
        UWorld* w = UWorld::GetWorld();
        if (!PtrOk(w) || !PtrOk(w->OwningGameInstance)) return nullptr;
        auto& lps = w->OwningGameInstance->LocalPlayers;
        if (lps.Num() <= 0 || !PtrOk(lps[0])) return nullptr;
        APlayerController* pc = lps[0]->PlayerController;
        if (!PtrOk(pc) || !pc->IsA(ABP_PlayerControllerOnline_C::StaticClass())) return nullptr;
        return static_cast<ABP_PlayerControllerOnline_C*>(pc);
    }
    UBP_ItemContainerComponent_C* Container(UObject* o) {
        return PtrOk(o) && o->IsA(UBP_ItemContainerComponent_C::StaticClass()) ? static_cast<UBP_ItemContainerComponent_C*>(o) : nullptr;
    }
}

namespace items::io {
    Located Locate() {
        Located w;
        ABP_PlayerControllerOnline_C* pc = LocalPC();
        if (!pc) { w.how = "no BP_PlayerControllerOnline_C"; return w; }
        UBP_InvManagerComponent_C* inv = PtrOk(pc->InvManagerComponent) ? pc->InvManagerComponent : nullptr;
        UBP_ItemContainerComponent_C* bank = Container(pc->ItemContainerStorage);  // the bank: a component on the controller
        w.how = bank ? "bank=pc.ItemContainerStorage" : "no bank container";
        w.pc = pc;
        w.inv = inv;
        w.bag = Container(pc->InventoryItemContainerComponent);
        w.bank = bank;
        return w;
    }

    std::vector<Item> Read(void* container, bool bank, bool stats) {
        std::vector<Item> out;
        auto* c = Container(static_cast<UObject*>(container));
        if (!c || !Ready()) return out;
        bool rebuilt = false;
        UClass* weapon = UBP_SpecItemWeapon_C::StaticClass();
        UClass* armor = UBP_SpecItemArmor_C::StaticClass();
        UClass* equipable = UBP_SpecItemEquipable_C::StaticClass();
        UClass* common = UBP_SpecItemCommon_C::StaticClass();
        for (int i = 0; i < c->Items.Num(); i++) {
            const FBP_ItemStruct& r = c->Items[i];
            Item it;
            it.bank = bank;
            it.containerType = uint8_t(r.ItemContainerType_26_91CD23B1402E40440F3A258CAC3AF44F);
            it.where = WhereOf(At(GetNames().container, it.containerType), bank);
            it.slot = r.ContainerSlot_11_662DB50B4C26F454F5A784B37A60C20B;
            it.specId = r.ItemSpecID_2_0F6087C54FAF6FCE287F81BEFD5CAC4B;
            it.grade = int(r.Itemgrade_29_AE6419044A6E394815070E8A0964ED01);
            it.level = r.ItemLevel_38_C9A8FF0246A556C2B5CC17A574AB792A;
            it.changeId = r.ChangedID_43_886257FA4C990C77EF6A2AB3D3CAA822;
            if (UArchonSpec* sp = Spec(it.specId, rebuilt)) {
                it.name = sp->GetName();
                if (sp->IsA(equipable)) it.equipSlot = int(static_cast<UBP_SpecItemEquipable_C*>(sp)->equipSlot);
                if (sp->IsA(weapon)) {
                    auto* wp = static_cast<UBP_SpecItemWeapon_C*>(sp);
                    it.kind = Kind::Weapon;
                    it.weaponType = int(wp->WeaponAnimationType);
                    it.typeName = wp->WeaponItemSpecData.mWeaponType_101_7C40707C4775D05C3C1AAB8DBA0BEA47.ToString();  // Wand; anim type says Club
                    if (it.typeName.empty() || it.typeName == "None") it.typeName = At(GetNames().weaponType, it.weaponType);
                    it.attack = AttackOfWeapon(it.typeName);
                    it.name = Text(wp->WeaponItemSpecData.mDisplayName_2_3062F8A64DE28500FF2B46B3C800B32B, it.name);
                } else if (sp->IsA(armor)) {
                    it.kind = Kind::Armor;
                    it.name = Text(static_cast<UBP_SpecItemArmor_C*>(sp)->ItemArmorSpecData.mDisplayName_2_3062F8A64DE28500FF2B46B3C800B32B, it.name);
                } else if (sp->IsA(common))
                    it.name = Text(static_cast<UBP_SpecItemCommon_C*>(sp)->ItemSpecCommonData.mDisplayName_2_77D264014CB57F0A282FA187B8C21C04, it.name);
            } else it.name = "spec " + I(it.specId);
            if (stats && it.kind != Kind::Other)
                if (UArchonAttributeSet_Secondary* set = AttrSet(c, it))
                    for (int k = 0; k < int(AttrOffsets().size()); k++) {
                        const float v = *reinterpret_cast<const float*>(reinterpret_cast<const uint8*>(set) + AttrOffsets()[k]);
                        if (v != 0) it.stats.push_back({k, v});
                    }
            out.push_back(std::move(it));
        }
        return out;
    }

    Bank ReadBank(bool stats) {
        Bank b;
        const Located w = Locate();
        auto* c = Container(static_cast<UObject*>(w.bank));
        if (c && c->Items.Num() > 0) {
            b.items = Read(c, true, stats);
            b.live = b.seen = true;
            AcquireSRWLockExclusive(&g_mu); g_bankCache = b.items; g_bankSeen = true; ReleaseSRWLockExclusive(&g_mu);
            return b;
        }
        AcquireSRWLockShared(&g_mu); b.items = g_bankCache; b.seen = g_bankSeen; ReleaseSRWLockShared(&g_mu);
        return b;
    }

    std::uint64_t Signature(void* container) {
        auto* c = Container(static_cast<UObject*>(container));
        if (!c) return 0;
        std::uint64_t h = 1469598103934665603ull;  // FNV-1a over the raw FBP_ItemStruct records
        const auto* p = reinterpret_cast<const uint8*>(c->Items.GetDataPtr());
        const size_t n = PtrOk(p) ? size_t(c->Items.Num()) * sizeof(FBP_ItemStruct) : 0;
        for (size_t i = 0; i < n; i++) h = (h ^ p[i]) * 1099511628211ull;
        return h ^ n;
    }
}

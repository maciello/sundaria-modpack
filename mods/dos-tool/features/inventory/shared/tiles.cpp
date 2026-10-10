#include "tiles.hpp"
#include "umg.hpp"

#include <algorithm>
#include "UMG_classes.hpp"
#include "WidgetItemBag_classes.hpp"
#include "WidgetItemInventory_classes.hpp"
#include "WidgetItemStorage_classes.hpp"
#include "WidgetItemIconContainer_classes.hpp"
#include "WidgetItemDisplayDetail_classes.hpp"
#include "BP_CharacterBase_classes.hpp"
#include "BP_PlayerControllerGame_classes.hpp"
#include "FItemContainerFunctions_classes.hpp"
#include "FItemContainerFunctions_parameters.hpp"

// Game thread only. Blueprint classes come and go with the map (#63): ref::Fn / ref::Cached re-resolve them.
using namespace SDK;
using umg::PtrOk;

namespace {
    ref::Fn g_convert{UFItemContainerFunctions_C::StaticClass, "FItemContainerFunctions_C", "ConvertCompressedItemSlot"};
    ref::Fn g_equipChanged{ABP_CharacterBase_C::StaticClass, "BP_CharacterBase_C", "FOnEquipContainerAttributeSetUpdate"};
    ref::Fn g_setChanged{ABP_PlayerControllerGame_C::StaticClass, "BP_PlayerControllerGame_C", "OnRep_WeaponMode"};
    ref::Fn g_detailTick{UWidgetItemDisplayDetail_C::StaticClass, "WidgetItemDisplayDetail_C", "Tick"};
    ref::Cached<UClass> g_bagCls{[] { return UWidgetItemBag_C::StaticClass(); }};
    ref::Cached<UClass> g_invCls{[] { return UWidgetItemInventory_C::StaticClass(); }};
    ref::Cached<UClass> g_storCls{[] { return UWidgetItemStorage_C::StaticClass(); }};

    items::tiles::Shown Of(const FSItemUIData& d) {
        return {d.SpecId_23_B842031D4333CC5CB157B2ACEF10803B, d.ChangeID_14_3C4F923F41B7BD67025418A6109516F6, d.IconID_2_B4E9648B461DD2F0023603B7C3264262};
    }
}

namespace items::tiles {
    void Listen(game::EventListener cb, bool on) {
        for (const char* cls : {"WidgetItemBag_C", "WidgetItemInventory_C", "WidgetItemStorage_C"}) game::OnClass(cls, cb, on);
        game::On("WidgetItemDisplayDetail_C", "Tick", cb, on);
        game::On("BP_CharacterBase_C", "FOnEquipContainerAttributeSetUpdate", cb, on);
        game::On("BP_PlayerControllerGame_C", "OnRep_WeaponMode", cb, on);
        game::OnWorldTick(cb, on);
    }

    Ev Classify(void* objp, void* fnp, void** what) {
        auto* obj = static_cast<UObject*>(objp);
        if (g_detailTick.Is(fnp)) { *what = obj; return Ev::Detail; }
        if (g_equipChanged.Is(fnp) || g_setChanged.Is(fnp)) { *what = obj; return Ev::Equip; }
        UClass* c = obj->Class;
        UClass* bagCls = g_bagCls.Get();
        UObject* bag = !bagCls              ? nullptr
                     : c == bagCls          ? obj
                     : c == g_invCls.Get()  ? static_cast<UWidgetItemInventory_C*>(obj)->widget_ItemBag
                     : c == g_storCls.Get() ? static_cast<UWidgetItemStorage_C*>(obj)->widget_ItemStorageBag
                                            : nullptr;
        if (!PtrOk(bag) || bag->Class != bagCls) return Ev::Other;
        *what = bag;
        return Ev::Bag;
    }

    bool Note(std::vector<ref::Ref>& seen, void* w) {
        for (const ref::Ref& t : seen) if (t.Is(w)) return false;
        seen.push_back(ref::Ref(w));
        return true;
    }
    void Prune(std::vector<ref::Ref>& seen) {
        std::erase_if(seen, [](const ref::Ref& t) { return !t.Get(); });
    }

    std::vector<void*> Slots(const std::vector<ref::Ref>& bags) {
        std::vector<void*> out;
        for (const ref::Ref& t : bags) {
            auto* bag = t.Get<UWidgetItemBag_C>();
            if (!bag) continue;
            auto& slots = bag->ItemContainers;
            for (int k = 0; k < slots.Num(); k++) {
                UWidgetItemIconContainer_C* c = slots[k];
                if (umg::Live(c) && PtrOk(c->Overlay_Container) && PtrOk(c->WidgetTree)) out.push_back(c);
            }
        }
        return out;
    }

    bool Locate(void* slot, Pos& out) {
        auto* c = static_cast<UWidgetItemIconContainer_C*>(slot);
        UFunction* fn = g_convert.Get();
        if (!fn) return false;
        Params::FItemContainerFunctions_C_ConvertCompressedItemSlot p{};
        p.CompressedItemSlot = c->CompressedItemSlot;
        p.__WorldContext = c;
        UFItemContainerFunctions_C::GetDefaultObj()->ProcessEvent(fn, &p);
        out = {c->IsStorage, std::uint8_t(p.ContainerType), p.ItemSlot};
        return true;
    }

    Shown OfSlot(void* slot) { return Of(static_cast<UWidgetItemIconContainer_C*>(slot)->ItemData); }
    Shown OfDetail(void* detail) { return Of(static_cast<UWidgetItemDisplayDetail_C*>(detail)->LoadedItemUIData); }
}

#include "items.hpp"
#include "logger.hpp"

#include <Windows.h>
#include <atomic>
#include <cstdint>
#include <unordered_map>
#include "Engine_classes.hpp"
#include "ArchonSpecSystem_classes.hpp"
#include "BP_PlayerControllerOnline_classes.hpp"
#include "BP_InvManagerComponent_classes.hpp"
#include "BP_ItemContainerComponent_classes.hpp"
#include "BP_ItemContainerComponent_parameters.hpp"
#include "BP_ItemContainerStorage_classes.hpp"
#include "BP_AccountItemStorage_classes.hpp"
#include "BP_SpecItemWeapon_classes.hpp"
#include "BP_SpecItemArmor_classes.hpp"
#include "BP_SpecItemCommon_classes.hpp"

// Item reads for every item feature (#16). Facts: references/game-facts.md § items.
using namespace SDK;

namespace {
    using namespace items;
    using namespace items::io;

    bool PtrOk(const void* p) {
        const uintptr_t v = reinterpret_cast<uintptr_t>(p);
        return v > 0x10000 && v < 0x7FFFFFFFFFFFull;
    }
    std::string I(long long v) { return std::to_string(v); }

    // Dumper-7's TMap iterator does not compile (SetElement::Value is private): walk the sparse array directly.
    template <class K, class V, class F> void ForEach(const TMap<K, V>& m, F&& f) {
        using Elem = UC::ContainerImpl::SetElement<UC::TPair<K, V>>;  // {TPair, HashNextId, HashIndex}
        const uint8* data = *reinterpret_cast<const uint8* const*>(&m);
        if (!PtrOk(data)) return;
        for (int i = 0; i < m.NumAllocated(); i++)
            if (m.IsValidIndex(i)) {
                auto& kv = *reinterpret_cast<const UC::TPair<K, V>*>(data + i * sizeof(Elem));
                f(kv.Key(), kv.Value());
            }
    }

    // Enum value → display name (BP enums are dumped as NewEnumeratorN; UUserDefinedEnum keeps the names).
    std::vector<std::string> EnumNames(const char* name) {
        std::vector<std::string> out;
        UEnum* e = UObject::FindObjectFast<UEnum>(name, EClassCastFlags::Enum);  // flag None never matches (HasTypeFlag)
        if (!PtrOk(e)) return out;
        std::unordered_map<std::string, std::string> display;
        if (e->IsA(UUserDefinedEnum::StaticClass()))
            ForEach(static_cast<UUserDefinedEnum*>(e)->DisplayNameMap, [&](const FName& k, const FText& t) {
                if (PtrOk(t.TextData)) display[k.ToString()] = t.ToString();
            });
        for (int i = 0; i < e->Names.Num(); i++) {
            std::string n = e->Names[i].Key().ToString();
            if (auto p = n.rfind("::"); p != std::string::npos) n = n.substr(p + 2);
            const int64 v = e->Names[i].Value();
            if (v < 0 || v > 255 || n.ends_with("_MAX")) continue;
            if (int(out.size()) <= v) out.resize(v + 1);
            auto it = display.find(n);
            out[v] = it != display.end() && !it->second.empty() ? it->second : n;
        }
        return out;
    }
    std::vector<std::string> EquipSlotNames() {
        std::vector<std::string> n = EnumNames("EBP_ItemEquipmentSlotEnum");
        return n.empty() ? EnumNames("BP_ItemEquipmentSlotEnum") : n;
    }
    std::string Text(const FText& t, const std::string& fallback) {
        const std::string s = PtrOk(t.TextData) ? t.ToString() : "";
        return s.empty() ? fallback : s;
    }
    std::string At(const std::vector<std::string>& v, int i) { return i >= 0 && i < int(v.size()) ? v[i] : ""; }

    // Written once on the render thread, then read-only (published by g_ready).
    Names g_names;
    std::vector<int32> g_attrOffset;  // UArchonAttributeSet_Secondary float property offsets, parallel to g_names.stat
    std::atomic<bool> g_ready{false};
    double g_nextTick = 0;

    void LoadNames() {
        Names n{EnumNames("EItemContainerType"), EnumNames("EWeaponType"), EquipSlotNames(), {}};
        std::vector<int32> offs;
        UClass* c = UArchonAttributeSet_Secondary::StaticClass();
        for (FField* f = PtrOk(c) ? c->ChildProperties : nullptr; PtrOk(f); f = f->Next)
            if (PtrOk(f->ClassPrivate) && f->ClassPrivate->Name.ToString() == "FloatProperty") {
                n.stat.push_back(f->Name.ToString());
                offs.push_back(static_cast<FProperty*>(f)->Offset);
            }
        logger::log("[items] names: containers " + I(n.container.size()) + ", weapon types " + I(n.weaponType.size()) +
                    ", equip slots " + I(n.equipSlot.size()) + ", item stat attributes " + I(n.stat.size()));
        if (n.container.empty() || n.stat.empty()) return;  // retried next tick
        g_names = std::move(n);
        g_attrOffset = std::move(offs);
        g_ready.store(true, std::memory_order_release);
    }

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

    // Bank candidates found by the render-thread scan: storage containers owned by the local controller.
    SRWLOCK g_mu = SRWLOCK_INIT;  // not std::mutex (gotchas). Guards g_bankScan, g_bankCache.
    struct Ref { UObject* o; int32 idx; };
    std::vector<Ref> g_bankScan;
    std::vector<Item> g_bankCache;
    bool g_bankSeen = false;
    bool Alive(const Ref& r) { return PtrOk(r.o) && UObject::GObjects->GetByIndex(r.idx) == r.o; }

    void ScanBank() {
        std::vector<Ref> banks;
        UClass* cls = UBP_ItemContainerStorage_C::StaticClass();
        APlayerController* pc = LocalPC();
        for (int i = 0; PtrOk(cls) && pc && i < UObject::GObjects->Num(); i++) {
            UObject* o = UObject::GObjects->GetByIndex(i);
            if (PtrOk(o) && o->IsA(cls) && !o->IsDefaultObject() && static_cast<UBP_ItemContainerComponent_C*>(o)->PlayerController == pc)
                banks.push_back({o, o->Index});
        }
        AcquireSRWLockExclusive(&g_mu); g_bankScan = std::move(banks); ReleaseSRWLockExclusive(&g_mu);
    }

    // ponytail: scans GObjects for spec managers per read (game thread, ~ms); cache if it hitches
    std::unordered_map<int, UArchonSpec*> SpecMap() {
        std::unordered_map<int, UArchonSpec*> out;
        UClass* cls = UArchonSpecManager::StaticClass();
        for (int i = 0; i < UObject::GObjects->Num(); i++) {
            UObject* o = UObject::GObjects->GetByIndex(i);
            if (!PtrOk(o) || !o->IsA(cls) || o->IsDefaultObject()) continue;
            ForEach(static_cast<UArchonSpecManager*>(o)->mLoadedSpecMap, [&](int32 id, UArchonSpec* sp) {
                if (PtrOk(sp)) out[id] = sp;
            });
        }
        return out;
    }

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

namespace items::io {
    void Tick() {
        const double now = GetTickCount64() / 1000.0;
        if (now < g_nextTick) return;
        g_nextTick = now + 1.0;
        if (!g_ready.load(std::memory_order_acquire)) LoadNames();
        ScanBank();
    }
    bool Ready() { return g_ready.load(std::memory_order_acquire); }
    const Names& GetNames() { return g_names; }

    Located Locate() {
        Located w;
        ABP_PlayerControllerOnline_C* pc = LocalPC();
        if (!pc) { w.how = "no BP_PlayerControllerOnline_C"; return w; }
        UBP_InvManagerComponent_C* inv = PtrOk(pc->InvManagerComponent) ? pc->InvManagerComponent : nullptr;
        UBP_ItemContainerComponent_C* bank = nullptr;
        if (inv) {
            if ((bank = Container(inv->PlayerPersistentComponent))) w.how = "bank=inv.PlayerPersistentComponent";
            else if (PtrOk(inv->ItemStorage) && (bank = Container(inv->ItemStorage->PlayerComponent))) w.how = "bank=inv.ItemStorage.PlayerComponent";
        }
        if (!bank) {
            AcquireSRWLockShared(&g_mu);
            for (const Ref& r : g_bankScan)
                if (Alive(r) && !bank) { bank = static_cast<UBP_ItemContainerComponent_C*>(r.o); w.how = "bank=scan(" + r.o->GetName() + ")"; }
            ReleaseSRWLockShared(&g_mu);
        }
        if (!bank) w.how = "no bank container";
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
        const auto specs = SpecMap();
        UClass* weapon = UBP_SpecItemWeapon_C::StaticClass();
        UClass* armor = UBP_SpecItemArmor_C::StaticClass();
        UClass* equipable = UBP_SpecItemEquipable_C::StaticClass();
        UClass* common = UBP_SpecItemCommon_C::StaticClass();
        for (int i = 0; i < c->Items.Num(); i++) {
            const FBP_ItemStruct& r = c->Items[i];
            Item it;
            it.bank = bank;
            it.containerType = uint8_t(r.ItemContainerType_26_91CD23B1402E40440F3A258CAC3AF44F);
            it.where = WhereOf(At(g_names.container, it.containerType), bank);
            it.slot = r.ContainerSlot_11_662DB50B4C26F454F5A784B37A60C20B;
            it.specId = r.ItemSpecID_2_0F6087C54FAF6FCE287F81BEFD5CAC4B;
            it.grade = int(r.Itemgrade_29_AE6419044A6E394815070E8A0964ED01);
            it.level = r.ItemLevel_38_C9A8FF0246A556C2B5CC17A574AB792A;
            it.changeId = r.ChangedID_43_886257FA4C990C77EF6A2AB3D3CAA822;
            if (auto s = specs.find(it.specId); s != specs.end()) {
                UArchonSpec* sp = s->second;
                it.name = sp->GetName();
                if (sp->IsA(equipable)) it.equipSlot = int(static_cast<UBP_SpecItemEquipable_C*>(sp)->equipSlot);
                if (sp->IsA(weapon)) {
                    auto* wp = static_cast<UBP_SpecItemWeapon_C*>(sp);
                    it.kind = Kind::Weapon;
                    it.weaponType = int(wp->WeaponAnimationType);
                    it.typeName = wp->WeaponItemSpecData.mWeaponType_101_7C40707C4775D05C3C1AAB8DBA0BEA47.ToString();  // Wand; anim type says Club
                    if (it.typeName.empty() || it.typeName == "None") it.typeName = At(g_names.weaponType, it.weaponType);
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
                    for (int k = 0; k < int(g_attrOffset.size()); k++) {
                        const float v = *reinterpret_cast<const float*>(reinterpret_cast<const uint8*>(set) + g_attrOffset[k]);
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

    std::string ContainersReport() {
        std::string out;
        APlayerController* pc = LocalPC();
        for (int i = 0; i < UObject::GObjects->Num(); i++) {
            UObject* o = UObject::GObjects->GetByIndex(i);
            UBP_ItemContainerComponent_C* c = PtrOk(o) && !o->IsDefaultObject() ? Container(o) : nullptr;
            if (!c || c->Items.Num() == 0) continue;
            out += "\n  " + o->Class->GetName() + " " + o->GetName() + " outer " + (PtrOk(o->Outer) ? o->Outer->GetName() : "-") +
                   ": Items " + I(c->Items.Num()) + (c->PlayerController == pc ? " (mine)" : "");
        }
        return out;
    }
}

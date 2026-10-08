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
#include "BP_SpecItemBase_parameters.hpp"

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

    SRWLOCK g_mu = SRWLOCK_INIT;  // not std::mutex (gotchas). Guards g_bankCache.
    std::vector<Item> g_bankCache;
    bool g_bankSeen = false;

    double Ms(LARGE_INTEGER a) {
        LARGE_INTEGER b, f;
        QueryPerformanceCounter(&b);
        QueryPerformanceFrequency(&f);
        return double(b.QuadPart - a.QuadPart) * 1000.0 / double(f.QuadPart);
    }

    // Spec id → spec, built by one GObjects walk; rebuilt only when an id misses or a cached spec died (game thread).
    struct SpecRef { UArchonSpec* sp; int32 idx; };
    std::unordered_map<int, SpecRef> g_specs;
    void BuildSpecs() {
        LARGE_INTEGER t0;
        QueryPerformanceCounter(&t0);
        g_specs.clear();
        UClass* cls = UArchonSpecManager::StaticClass();
        for (int i = 0; i < UObject::GObjects->Num(); i++) {
            UObject* o = UObject::GObjects->GetByIndex(i);
            if (!PtrOk(o) || !o->IsA(cls) || o->IsDefaultObject()) continue;
            ForEach(static_cast<UArchonSpecManager*>(o)->mLoadedSpecMap, [&](int32 id, UArchonSpec* sp) {
                if (PtrOk(sp)) g_specs[id] = {sp, sp->Index};
            });
        }
        logger::log("[items] spec map rebuilt: " + I(g_specs.size()) + " specs, " + std::to_string(Ms(t0)).substr(0, 5) + " ms");
    }
    UArchonSpec* Spec(int id, bool& rebuilt) {
        auto it = g_specs.find(id);
        const bool ok = it != g_specs.end() && UObject::GObjects->GetByIndex(it->second.idx) == it->second.sp;
        if (ok) return it->second.sp;
        if (rebuilt) return nullptr;  // one rebuild per read
        rebuilt = true;
        BuildSpecs();
        it = g_specs.find(id);
        return it != g_specs.end() ? it->second.sp : nullptr;
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

namespace items::profiles {
    namespace {
        SRWLOCK mu = SRWLOCK_INIT;
        std::vector<Profile> all = Presets();
        std::atomic<int> active{0};
    }
    std::vector<Profile> All() {
        AcquireSRWLockShared(&mu); std::vector<Profile> v = all; ReleaseSRWLockShared(&mu);
        return v;
    }
    void SetAll(std::vector<Profile> v) {
        if (v.empty()) return;
        AcquireSRWLockExclusive(&mu); all = std::move(v); ReleaseSRWLockExclusive(&mu);
        SetActive(active.load());
    }
    int ActiveIndex() { return active.load(); }
    void SetActive(int i) {
        AcquireSRWLockShared(&mu); const int n = int(all.size()); ReleaseSRWLockShared(&mu);
        active = std::clamp(i, 0, n - 1);
    }
    Profile Active() {
        AcquireSRWLockShared(&mu); Profile p = all[std::clamp(active.load(), 0, int(all.size()) - 1)]; ReleaseSRWLockShared(&mu);
        return p;
    }
}

namespace items::io {
    void Tick() {
        const double now = GetTickCount64() / 1000.0;
        if (now < g_nextTick) return;
        g_nextTick = now + 1.0;
        if (!g_ready.load(std::memory_order_acquire)) LoadNames();
    }
    bool Ready() { return g_ready.load(std::memory_order_acquire); }
    const Names& GetNames() { return g_names; }

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
            it.where = WhereOf(At(g_names.container, it.containerType), bank);
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

    bool CanSalvage(int specId) {
        static std::unordered_map<int, bool> cache;
        if (auto it = cache.find(specId); it != cache.end()) return it->second;
        bool rebuilt = false;
        UArchonSpec* sp = Spec(specId, rebuilt);
        bool can = false;
        if (sp && sp->IsA(UBP_SpecItemBase_C::StaticClass()))
            if (UFunction* fn = sp->Class->GetFunction("BP_SpecItemBase_C", "I_CanSalvage")) {
                Params::BP_SpecItemBase_C_I_CanSalvage p{};
                sp->ProcessEvent(fn, &p);
                can = p.CanSalvage;
            }
        return cache[specId] = can;
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

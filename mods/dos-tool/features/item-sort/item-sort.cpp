#include "feature.hpp"
#include "item-sort.hpp"
#include "inventory-ui.hpp"
#include "logger.hpp"
#include "imgui.h"
#include "imgui_internal.h"  // MarkIniSettingsDirty

#include <Windows.h>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <unordered_map>
#include "Engine_classes.hpp"
#include "ArchonSpecSystem_classes.hpp"
#include "BP_PlayerControllerOnline_classes.hpp"
#include "BP_InvManagerComponent_classes.hpp"
#include "BP_InvManagerComponent_parameters.hpp"
#include "BP_ItemContainerComponent_classes.hpp"
#include "BP_ItemContainerComponent_parameters.hpp"
#include "BP_ItemContainerStorage_classes.hpp"
#include "BP_AccountItemStorage_classes.hpp"
#include "BP_SpecItemWeapon_classes.hpp"
#include "BP_SpecItemArmor_classes.hpp"
#include "BP_SpecItemCommon_classes.hpp"
#include "WidgetitemBagHeaderMenu_classes.hpp"
#include "BP_HUDInventoryComponent_classes.hpp"
#include "FItemContainerFunctions_classes.hpp"
#include "FItemContainerFunctions_parameters.hpp"
#include "FItemSortFunctions_classes.hpp"
#include "FItemSortFunctions_parameters.hpp"

// Item sort (non-visual part; the inventory UI is the designer's): the game's own Sort button sorts the bag
// by the active profile (item-sort.hpp: score, order). Game thread (ProcessEvent listener): read items +
// stats, compute our order, apply it through the game's reorder UFunction, read the container back.
// Render thread: enum/attribute names, bank lookup, dev triggers. API for the UI: item_sort::api (item-sort.hpp).
// Dev loop: files next to the exe trigger work once: item-sort.probe (log only), item-sort.profile<i>, item-sort.apply / item-sort.apply-bank.
using namespace SDK;

namespace {
    using namespace item_sort;

    inline bool PtrOk(const void* p) {
        const uintptr_t v = reinterpret_cast<uintptr_t>(p);
        return v > 0x10000 && v < 0x7FFFFFFFFFFFull;
    }
    struct RawArray { void* data; int32 num, max; };  // TArray layout
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
    std::string Text(const FText& t, const std::string& fallback) {
        const std::string s = PtrOk(t.TextData) ? t.ToString() : "";
        return s.empty() ? fallback : s;
    }
    std::string At(const std::vector<std::string>& v, int i) { return i >= 0 && i < int(v.size()) ? v[i] : ""; }

    std::vector<std::string> EquipSlotNames() {
        std::vector<std::string> n = EnumNames("EBP_ItemEquipmentSlotEnum");
        return n.empty() ? EnumNames("BP_ItemEquipmentSlotEnum") : n;
    }

    // Written once on the render thread, then read-only (published by g_haveNames).
    struct Attr { std::string name; int32 offset; };
    struct Names {
        std::vector<std::string> container, weaponType, equipSlot;
        std::vector<Attr> attr;          // float properties of UArchonAttributeSet_Secondary = item stats
        std::vector<std::string> stat;   // attr names, Stat::type indexes this
    } g_names;
    std::atomic<bool> g_haveNames{false};

    std::vector<Attr> AttrFloats() {
        std::vector<Attr> out;
        UClass* c = UArchonAttributeSet_Secondary::StaticClass();
        for (FField* f = PtrOk(c) ? c->ChildProperties : nullptr; PtrOk(f); f = f->Next)
            if (PtrOk(f->ClassPrivate) && f->ClassPrivate->Name.ToString() == "FloatProperty")
                out.push_back({f->Name.ToString(), static_cast<FProperty*>(f)->Offset});
        return out;
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
    SRWLOCK g_mu = SRWLOCK_INIT;  // not std::mutex (gotchas). Guards g_bankScan, g_status, g_profile.
    struct Ref { UObject* o; int32 idx; };
    std::vector<Ref> g_bankScan;
    bool Alive(const Ref& r) { return PtrOk(r.o) && UObject::GObjects->GetByIndex(r.idx) == r.o; }

    struct Where2 { ABP_PlayerControllerOnline_C* pc; UBP_InvManagerComponent_C* inv; UBP_ItemContainerComponent_C* bag; UBP_ItemContainerComponent_C* bank; std::string how; };
    Where2 Locate() {
        Where2 w{LocalPC(), nullptr, nullptr, nullptr, ""};
        if (!w.pc) { w.how = "no BP_PlayerControllerOnline_C"; return w; }
        w.inv = PtrOk(w.pc->InvManagerComponent) ? w.pc->InvManagerComponent : nullptr;
        w.bag = Container(w.pc->InventoryItemContainerComponent);
        if (w.inv) {
            if ((w.bank = Container(w.inv->PlayerPersistentComponent))) w.how = "bank=inv.PlayerPersistentComponent";
            else if (PtrOk(w.inv->ItemStorage) && (w.bank = Container(w.inv->ItemStorage->PlayerComponent))) w.how = "bank=inv.ItemStorage.PlayerComponent";
        }
        if (!w.bank) {
            AcquireSRWLockShared(&g_mu);
            for (const Ref& r : g_bankScan)
                if (Alive(r) && !w.bank) { w.bank = static_cast<UBP_ItemContainerComponent_C*>(r.o); w.how = "bank=scan(" + r.o->GetName() + ")"; }
            ReleaseSRWLockShared(&g_mu);
        }
        if (!w.bank) w.how = "no bank container (open the bank once?)";
        return w;
    }

    // ponytail: scans GObjects for spec managers per sort (game thread, ~ms); cache if it hitches
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

    // ---- game thread ----
    thread_local bool t_busy = false;  // our own UFunction calls re-enter ProcessEvent
    std::string g_status;
    std::atomic<int> g_active{0};
    std::vector<Profile> g_profiles = Presets();  // weights from dos-tool.ini; guarded by g_mu
    std::atomic<unsigned> g_request{0};           // 1 inventory, 2 bank (API, dev files)
    std::atomic<bool> g_probe{false}, g_on{false};
    std::unordered_map<UFunction*, std::string> g_triggers;  // read-only once g_on
    unsigned g_pendingMask = 0;  // vanilla sort seen: 1 inventory, 2 bank
    ULONGLONG g_due = 0;

    void SetStatus(const std::string& s) {
        AcquireSRWLockExclusive(&g_mu); g_status = s; ReleaseSRWLockExclusive(&g_mu);
        logger::log("[item-sort] " + s);
    }
    std::vector<int> ReadInts(const TArray<int32>& a) {
        const RawArray& r = reinterpret_cast<const RawArray&>(a);
        std::vector<int> v;
        if (r.num > 0 && r.num < 100000 && PtrOk(r.data)) v.assign(static_cast<int32*>(r.data), static_cast<int32*>(r.data) + r.num);
        return v;
    }
    std::string Head(const std::vector<int>& v, int n = 16) {
        std::string s;
        for (int i = 0; i < int(v.size()) && i < n; i++) s += " " + I(v[i]);
        return s + (int(v.size()) > n ? " …" : "");
    }
    // Item identity across a reorder. Not ChangedID: the game rewrites it on every reorder.
    long long KeyOf(const Item& it) { return (static_cast<long long>(it.specId) << 32) | static_cast<unsigned>(it.level); }

    UFunction* Fn(UObject* o, const char* cls, const char* name) { return PtrOk(o) ? o->Class->GetFunction(cls, name) : nullptr; }

    UArchonAttributeSet_Secondary* AttrSet(UBP_ItemContainerComponent_C* c, const Item& it) {
        static UFunction* fn = nullptr;
        if (!fn) fn = Fn(c, "BP_ItemContainerComponent_C", "GetItemAttributeSet");
        if (!fn) return nullptr;
        Params::BP_ItemContainerComponent_C_GetItemAttributeSet p{};
        p.ItemSlot = it.slot;
        p.ContainerType = EItemContainerType(it.containerType);
        p.ForceRecalculate = false;
        c->ProcessEvent(fn, &p);
        return PtrOk(p.SecondaryAttributeSet) ? p.SecondaryAttributeSet : nullptr;
    }

    // Items of one container (memory), plus stats from the item's attribute set (UFunction: game thread).
    std::vector<Item> ReadItems(UBP_ItemContainerComponent_C* c, bool bank, bool stats, const std::unordered_map<int, UArchonSpec*>& specs) {
        std::vector<Item> out;
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
                    it.typeName = wp->WeaponItemSpecData.mWeaponType_101_7C40707C4775D05C3C1AAB8DBA0BEA47.ToString();  // Wand, not the anim type Club
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
                    for (int k = 0; k < int(g_names.attr.size()); k++) {
                        const float v = *reinterpret_cast<const float*>(reinterpret_cast<const uint8*>(set) + g_names.attr[k].offset);
                        if (v != 0) it.stats.push_back({k, v});
                    }
            out.push_back(std::move(it));
        }
        return out;
    }

    void LogItems(const char* what, const std::vector<Item>& items) {
        std::map<int, int> perType;
        int withStats = 0, att[4] = {}, kinds[3] = {};
        for (const Item& it : items) {
            perType[it.containerType]++;
            withStats += !it.stats.empty();
            kinds[int(it.kind)]++;
            if (it.kind == Kind::Weapon) att[int(it.attack)]++;
        }
        std::string s = std::string("[item-sort] ") + what + " " + I(items.size()) + " items:";
        for (auto& [t, n] : perType) s += " type" + I(t) + "'" + At(g_names.container, t) + "'=" + I(n);
        s += " | weapons " + I(kinds[1]) + " armor " + I(kinds[2]) + " other " + I(kinds[0]) + " | with stats " + I(withStats) +
             " | melee " + I(att[1]) + " ranged " + I(att[2]) + " magic " + I(att[3]) + " unknown " + I(att[0]);
        logger::log(s);
        for (const Item& it : items)
            if (!it.stats.empty()) {
                std::string st;
                for (const Stat& x : it.stats) st += " " + StatName(g_names.stat, x.type) + "=" + std::to_string(x.value).substr(0, 6);
                logger::log("[item-sort]   e.g. " + it.name + " slot " + I(it.slot) + " lv " + I(it.level) + ":" + st);
                break;
            }
    }

    // Main bag only (DefaultContainer = type 0; equipped/temp types stay where they are).
    std::vector<Item> Bag(std::vector<Item> all) {
        std::vector<Item> out;
        for (Item& it : all) if (it.containerType == 0) out.push_back(std::move(it));
        return out;
    }

    // Order of the bag right now: item keys by ascending slot.
    std::vector<long long> CurrentOrder(UBP_ItemContainerComponent_C* c, bool bank, const std::unordered_map<int, UArchonSpec*>& specs) {
        std::vector<Item> items = Bag(ReadItems(c, bank, false, specs));
        std::sort(items.begin(), items.end(), [](const Item& a, const Item& b) { return a.slot < b.slot; });
        std::vector<long long> keys;
        for (const Item& it : items) keys.push_back(KeyOf(it));
        return keys;
    }

    // "slot=key" for every bag item, ascending slot: lets the log prove what a reorder call did.
    std::string Layout(UBP_ItemContainerComponent_C* c, bool bank, const std::unordered_map<int, UArchonSpec*>& specs) {
        std::vector<Item> items = Bag(ReadItems(c, bank, false, specs));
        std::sort(items.begin(), items.end(), [](const Item& a, const Item& b) { return a.slot < b.slot; });
        std::string out;
        char buf[48];
        for (const Item& it : items) { std::snprintf(buf, sizeof buf, " %d=%llx", it.slot, static_cast<unsigned long long>(KeyOf(it))); out += buf; }
        return out;
    }

    void CallWithArray(UObject* obj, UFunction* fn, void* parms, TArray<int32>& field, const std::vector<int>& v) {
        RawArray arr{const_cast<int*>(v.data()), int32(v.size()), int32(v.size())};
        std::memcpy(&field, &arr, sizeof(arr));
        obj->ProcessEvent(fn, parms);
        std::memset(&field, 0, sizeof(arr));  // our memory, not the game's
    }

    // The game's sort list for this bag (its own format): FItemSortFunctions_C::SortItems.
    std::vector<int> GameSortList(UBP_ItemContainerComponent_C* c, UObject* ctx) {
        UFunction* fn = UFItemSortFunctions_C::StaticClass()->GetFunction("FItemSortFunctions_C", "SortItems");
        if (!fn) return {};
        // ponytail: the out TArray is allocated by the game and leaked (a few hundred bytes per sort)
        Params::FItemSortFunctions_C_SortItems p{};
        p.ItemContainer = c;
        p.ContainerType = EItemContainerType(0);
        p.Sort = EItemSort(0);
        p.Descending = false;
        p.__WorldContext = ctx;
        UFItemSortFunctions_C::GetDefaultObj()->ProcessEvent(fn, &p);
        return ReadInts(p.SorteditemSlots);
    }

    // Sort one bag by the active profile; every step logged, success proven by reading the bag back.
    void RunSort(bool bank) {
        const char* what = bank ? "bank" : "inventory";
        Where2 w = Locate();
        logger::log(std::string("[item-sort] sort ") + what + ": pc " + (w.pc ? "ok" : "-") + ", inv manager " + (w.inv ? "ok" : "-") +
                    ", bag " + (w.bag ? I(w.bag->Items.Num()) : "-") + ", " + w.how);
        UBP_ItemContainerComponent_C* c = bank ? w.bank : w.bag;
        if (!c || !w.inv) return SetStatus(std::string(what) + ": container not found (" + w.how + ")");
        AcquireSRWLockShared(&g_mu);
        const Profile prof = g_profiles[std::clamp(g_active.load(), 0, int(g_profiles.size()) - 1)];
        ReleaseSRWLockShared(&g_mu);

        const auto specs = SpecMap();
        std::vector<Item> items = Bag(ReadItems(c, bank, true, specs));
        LogItems(what, items);
        if (items.size() < 2) return SetStatus(std::string(what) + ": nothing to sort");
        std::vector<float> score;
        const std::vector<int> idx = Order(items, prof, g_names.stat, &score);
        std::vector<long long> intended;
        std::vector<int> bySlots;
        for (int i : idx) { intended.push_back(KeyOf(items[i])); bySlots.push_back(items[i].slot); }
        logger::log(std::string("[item-sort] profile '") + prof.name + "' order (group/bucket name score lv):");
        for (int k = 0; k < int(idx.size()) && k < 20; k++) {
            const Item& it = items[idx[k]];
            const Bucket bk = BucketOf(it, prof.focus);
            const std::string sub = bk.group == 0 ? it.typeName : bk.group == 3 ? "" : At(g_names.equipSlot, it.equipSlot) + "#" + I(it.equipSlot);
            logger::log("[item-sort]   " + I(k + 1) + ". " + kKindName[int(it.kind)] + (bk.group == 2 ? "(equipable)" : "") + "/" + sub + " '" +
                        it.name + "' " + std::to_string(score[idx[k]]).substr(0, 5) + " lv" + I(it.level));
        }

        // Try the game's reorder paths until the bag reads back in our order.
        struct Try { const char* name; std::vector<int> list; int how; };
        std::vector<Try> tries;
        tries.push_back({"InvManager.ReorderItems(slots in new order)", bySlots, 0});
        tries.push_back({"Container.Request_ReorderItems(slots in new order)", bySlots, 1});
        tries.push_back({"Container.RemapItemSlots(slots in new order)", bySlots, 2});
        const std::vector<long long> before = CurrentOrder(c, bank, specs);
        {
            std::string keys;
            char buf[24];
            for (long long k : intended) { std::snprintf(buf, sizeof buf, " %llx", static_cast<unsigned long long>(k)); keys += buf; }
            std::vector<long long> u = intended;
            std::sort(u.begin(), u.end());
            logger::log("[item-sort] intended keys (" + I(int(std::unique(u.begin(), u.end()) - u.begin())) + " unique):" + keys);
            logger::log("[item-sort] layout before:" + Layout(c, bank, specs));
        }
        for (const Try& t : tries) {
            if (t.how == 0) {
                UFunction* fn = Fn(w.inv, "BP_InvManagerComponent_C", "ReorderItems");
                if (!fn) continue;
                Params::BP_InvManagerComponent_C_ReorderItems p{};
                p.IsStorage = bank;
                CallWithArray(w.inv, fn, &p, p.SlotsToMove, t.list);
            } else if (t.how == 1) {
                UFunction* fn = Fn(c, "BP_ItemContainerComponent_C", "Request_ReorderItems");
                if (!fn) continue;
                Params::BP_ItemContainerComponent_C_Request_ReorderItems p{};
                p.ContainerType = EItemContainerType(0);
                CallWithArray(c, fn, &p, p.RemappedItemSlots, t.list);
            } else {
                UFunction* fn = Fn(c, "BP_ItemContainerComponent_C", "RemapItemSlots");
                if (!fn) continue;
                Params::BP_ItemContainerComponent_C_RemapItemSlots p{};
                p.ContainerType = EItemContainerType(0);
                CallWithArray(c, fn, &p, p.NewItemSlots, t.list);
            }
            const std::vector<long long> after = CurrentOrder(c, bank, specs);
            const int ok = InOrder(intended, after), was = InOrder(intended, before);
            logger::log("[item-sort] full list:" + Head(t.list, 1 << 20));
            logger::log("[item-sort] layout after:" + Layout(c, bank, specs));
            logger::log(std::string("[item-sort] tried ") + t.name + ": list" + Head(t.list, 8) + " -> in intended order " + I(ok) + "/" +
                        I(intended.size()) + " (before " + I(was) + "), bag changed " + (after != before ? "yes" : "no"));
            if (ok == int(intended.size()))
                return SetStatus(std::string(what) + ": sorted by '" + prof.name + "' via " + t.name);
            if (after != before)  // the bag moved but not into our order: stop, the next list was built for the old layout
                return SetStatus(std::string(what) + ": " + t.name + " moved items but not into the profile order (see log)");
        }
        SetStatus(std::string(what) + ": no reorder path produced the profile order (see log)");
    }

    void Probe() {
        Where2 w = Locate();
        // Every live item container: where the bank's items really are.
        APlayerController* lpc = LocalPC();
        for (int i = 0; i < UObject::GObjects->Num(); i++) {
            UObject* o = UObject::GObjects->GetByIndex(i);
            UBP_ItemContainerComponent_C* c = PtrOk(o) && !o->IsDefaultObject() ? Container(o) : nullptr;
            if (!c) continue;
            logger::log("[item-sort] probe container " + o->Class->GetName() + " " + o->GetName() + " outer " +
                        (PtrOk(o->Outer) ? o->Outer->GetName() : "-") + ": Items " + I(c->Items.Num()) + " size " + I(c->ContainerSize) +
                        " mine " + (c->PlayerController == lpc ? "yes" : "no"));
        }
        logger::log(std::string("[item-sort] probe: pc ") + (w.pc ? w.pc->Class->GetName() : "-") + ", inv " + (w.inv ? "ok" : "-") + ", " + w.how);
        if (w.inv)
            logger::log("[item-sort] probe: inv.PlayerPersistentComponent " + std::string(PtrOk(w.inv->PlayerPersistentComponent) ? "set" : "null") +
                        ", inv.ItemStorage " + (PtrOk(w.inv->ItemStorage) ? "set" : "null") + ", OpenStorage " + I(w.inv->OpenStorage));
        for (auto* c : {w.bag, w.bank}) {
            if (!c) continue;
            logger::log("[item-sort] probe " + c->GetName() + ": Items " + I(c->Items.Num()) + " ItemStatList " + I(c->ItemStatList.Num()) +
                        " ItemAttributeSet " + I(c->ItemAttributeSet.Num()) + " WeaponData " + I(c->WeaponData.Num()) + " ArmorData " +
                        I(c->ArmorData.Num()) + " SortedSimplified " + I(c->SortedSimplifiedItems.Num()) + " ContainerSize " + I(c->ContainerSize) +
                        " MaxItemSlot " + I(c->MaxItemSlot));
            const auto specs = SpecMap();
            LogItems(c == w.bag ? "probe bag" : "probe bank", ReadItems(c, c == w.bank, true, specs));
            logger::log("[item-sort] probe layout:" + Layout(c, c == w.bank, specs));
            logger::log("[item-sort] probe game sorter list:" + Head(GameSortList(c, w.inv ? static_cast<UObject*>(w.inv) : c), 24));
        }
    }

    void OnEvent(void* objp, void* fnp, void* parms) {
        if (t_busy || !g_on.load(std::memory_order_relaxed)) return;
        const ULONGLONG now = GetTickCount64();
        if (auto it = g_triggers.find(static_cast<UFunction*>(fnp)); it != g_triggers.end()) {
            auto* obj = static_cast<UObject*>(objp);
            const int off = StorageParamOffset(it->second);
            bool storage = false;
            if (off >= 0 && parms) storage = static_cast<const bool*>(parms)[off];
            else if (PtrOk(obj) && obj->IsA(UWidgetitemBagHeaderMenu_C::StaticClass())) storage = static_cast<UWidgetitemBagHeaderMenu_C*>(obj)->IsStorage;
            g_pendingMask |= storage ? 2 : 1;
            g_due = now + kSettleMs;
            logger::log("[item-sort] vanilla sort seen: " + it->second + (storage ? " (bank)" : " (inventory)"));
        }
        const bool probe = g_probe.exchange(false);
        const unsigned req = g_request.exchange(0);
        const unsigned todo = req | (g_pendingMask && now >= g_due ? g_pendingMask : 0);
        if (!probe && !todo) return;
        t_busy = true;
        if (probe) Probe();
        for (int b = 0; b < 2; b++)
            if (todo & (1u << b)) RunSort(b == 1);
        if (todo & g_pendingMask) g_pendingMask = 0;
        t_busy = false;
    }

    // Render thread, once: functions that start the game's own sort.
    bool Resolve() {
        UClass* classes[] = {UWidgetitemBagHeaderMenu_C::StaticClass(), UBP_HUDInventoryComponent_C::StaticClass(), UBP_InvManagerComponent_C::StaticClass()};
        std::string names;
        for (UClass* c : classes) {
            if (!PtrOk(c)) return false;
            for (UField* f = c->Children; PtrOk(f); f = f->Next) {
                const std::string n = f->GetName();
                if (f->IsA(UFunction::StaticClass()) && SortTrigger(n)) { g_triggers[static_cast<UFunction*>(f)] = n; names += " " + n; }
            }
        }
        logger::log("[item-sort] sort triggers:" + names);
        return !g_triggers.empty();
    }

    struct ItemSort : feature::Feature {
        bool resolved = false;
        double nextScan = 0;
        int savedActive = -1;
        std::string exeDir;

        ItemSort() : Feature("Item sort", feature::Stage::Alpha) { optIn = true; }  // new game-thread hook

        void Off() override {
            g_on = false;
            game::SetEventListener(&OnEvent, false);
            item_sort::ui::Off();
        }

        // profile=<i> | w.<profile>.<stat>=<weight> | a.<profile>.<attack>=<weight>
        void Load(const char* key, const char* value) override {
            const std::string k = key;
            AcquireSRWLockExclusive(&g_mu);
            if (k == "profile") g_active = std::clamp(std::atoi(value), 0, int(g_profiles.size()) - 1);
            const size_t dot = k.find('.', 2);
            if (k.size() >= 3 && k[1] == '.' && dot != std::string::npos)
                for (Profile& p : g_profiles) {
                    if (p.name != k.substr(2, dot - 2)) continue;
                    const float v = std::clamp(float(std::atof(value)), 0.0f, 2.0f);
                    if (k[0] == 'w') p.weight[k.substr(dot + 1)] = v;
                    if (k[0] == 'a') p.attack[std::atoi(k.c_str() + dot + 1)] = v;
                }
            ReleaseSRWLockExclusive(&g_mu);
        }
        void Save(std::vector<std::pair<std::string, std::string>>& out) override {
            AcquireSRWLockShared(&g_mu);
            out.push_back({"profile", I(g_active.load())});
            for (const Profile& p : g_profiles) {
                for (auto& [s, w] : p.weight) out.push_back({"w." + p.name + "." + s, std::to_string(w)});
                for (auto& [a, w] : p.attack) out.push_back({"a." + p.name + "." + I(a), std::to_string(w)});
            }
            ReleaseSRWLockShared(&g_mu);
        }

        void LoadNames() {
            Names n{EnumNames("EItemContainerType"), EnumNames("EWeaponType"), EquipSlotNames(), AttrFloats(), {}};
            for (const Attr& a : n.attr) n.stat.push_back(a.name);
            logger::log("[item-sort] names: containers " + I(n.container.size()) + ", weapon types " + I(n.weaponType.size()) + ", equip slots " + I(n.equipSlot.size()) +
                        ", item stat attributes " + I(n.attr.size()));
            if (n.container.empty() || n.attr.empty()) return;  // retried next scan
            g_names = std::move(n);
            g_haveNames.store(true, std::memory_order_release);
        }

        bool TakeFile(const char* name) {
            const std::string p = exeDir + name;
            if (GetFileAttributesA(p.c_str()) == INVALID_FILE_ATTRIBUTES) return false;
            DeleteFileA(p.c_str());
            logger::log(std::string("[item-sort] dev trigger ") + name);
            return true;
        }

        // Render thread: memory reads only; UFunction work happens in OnEvent (game thread).
        void OnFrame(const feature::Frame& f) override {
            if (savedActive != g_active.load()) { if (savedActive >= 0) ImGui::MarkIniSettingsDirty(); savedActive = g_active.load(); }
            if (f.now < nextScan) return;
            nextScan = f.now + 1.0;
            if (exeDir.empty()) {
                char buf[MAX_PATH] = {};
                GetModuleFileNameA(nullptr, buf, MAX_PATH);
                exeDir = buf;
                exeDir = exeDir.substr(0, exeDir.find_last_of("\\/") + 1);
            }
            if (!g_haveNames.load(std::memory_order_acquire)) LoadNames();
            if (!g_haveNames.load(std::memory_order_acquire)) return;
            if (!resolved && (resolved = Resolve())) { g_on = true; game::SetEventListener(&OnEvent, true); }
            if (resolved) item_sort::ui::Frame();  // game buttons in the inventory/bank header (inventory-ui.cpp)

            // Bank: storage containers owned by the local controller (memory scan, every second).
            std::vector<Ref> banks;
            UClass* cls = UBP_ItemContainerStorage_C::StaticClass();
            APlayerController* pc = LocalPC();
            for (int i = 0; PtrOk(cls) && pc && i < UObject::GObjects->Num(); i++) {
                UObject* o = UObject::GObjects->GetByIndex(i);
                if (PtrOk(o) && o->IsA(cls) && !o->IsDefaultObject() && static_cast<UBP_ItemContainerComponent_C*>(o)->PlayerController == pc)
                    banks.push_back({o, o->Index});
            }
            AcquireSRWLockExclusive(&g_mu); g_bankScan = std::move(banks); ReleaseSRWLockExclusive(&g_mu);

            if (TakeFile("item-sort.probe")) g_probe = true;
            for (int i = 0; i < 4; i++)
                if (TakeFile(("item-sort.profile" + std::to_string(i)).c_str())) api::SetActiveProfile(i);
            if (TakeFile("item-sort.apply")) g_request |= 1;
            if (TakeFile("item-sort.apply-bank")) g_request |= 2;
        }
    } g_item_sort;
}

namespace item_sort::api {
    std::vector<std::string> ProfileNames() {
        std::vector<std::string> out;
        AcquireSRWLockShared(&g_mu);
        for (const Profile& p : g_profiles) out.push_back(p.name);
        ReleaseSRWLockShared(&g_mu);
        return out;
    }
    int ActiveProfile() { return g_active.load(); }
    void SetActiveProfile(int i) { g_active = std::clamp(i, 0, int(Presets().size()) - 1); }
    void RequestSort(bool bank) { g_request |= bank ? 2u : 1u; }
    std::string LastStatus() {
        AcquireSRWLockShared(&g_mu);
        std::string s = g_status;
        ReleaseSRWLockShared(&g_mu);
        return s;
    }
}

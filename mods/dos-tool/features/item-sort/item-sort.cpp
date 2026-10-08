#include "feature.hpp"
#include "item-sort.hpp"
#include "logger.hpp"
#include "style.hpp"
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
#include "BP_ItemContainerStorage_classes.hpp"
#include "BP_AccountItemStorage_classes.hpp"
#include "BP_SpecItemWeapon_classes.hpp"
#include "BP_SpecItemArmor_classes.hpp"
#include "BP_PlayerControllerGame_classes.hpp"
#include "WidgetitemBagHeaderMenu_classes.hpp"
#include "WidgetItemInventory_classes.hpp"
#include "WidgetItemBag_classes.hpp"
#include "BP_HUDInventoryComponent_classes.hpp"
#include "UMG_classes.hpp"
#include "UMG_parameters.hpp"
#include "FItemContainerFunctions_classes.hpp"
#include "FItemContainerFunctions_parameters.hpp"

// Item sort, inside the game's inventory UI: the game's own Sort (bag header button, X key) is followed by a
// reorder by the active profile (item-sort.hpp: score, filter-first) through the game's ReorderItems; profile,
// weights and "first" filter sit in a strip anchored to the game's bag header while the inventory is open.
// Game thread (ProcessEvent listener): sort, widget geometry. Render thread: model, enum names, drawing.
using namespace SDK;

namespace {
    using namespace item_sort;

    inline bool PtrOk(const void* p) {
        const uintptr_t v = reinterpret_cast<uintptr_t>(p);
        return v > 0x10000 && v < 0x7FFFFFFFFFFFull;
    }
    struct RawArray { void* data; int32 num, max; };  // TArray layout

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
    std::string At(const std::vector<std::string>& v, int i) { return i >= 0 && i < int(v.size()) ? v[i] : ""; }

    // Written once on the render thread, then read-only (published by g_haveNames).
    struct Names { std::vector<std::string> stat, container, weaponType, damageType; } g_names;
    std::atomic<bool> g_haveNames{false};

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
    UBP_ItemContainerComponent_C* BankOf(UBP_InvManagerComponent_C* inv) {
        if (auto* c = Container(inv->PlayerPersistentComponent)) return c;
        ABP_AccountItemStorage_C* s = inv->ItemStorage;
        return PtrOk(s) ? Container(s->PlayerComponent) : nullptr;
    }

    // ponytail: scans GObjects for spec managers on every rebuild (menu open, 2/s); cache if it shows in frame time
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

    void ReadContainer(UBP_ItemContainerComponent_C* c, bool bank, const std::unordered_map<int, UArchonSpec*>& specs,
                       std::vector<Item>& out) {
        auto key = [](int type, int slot) { return (int64_t(type) << 32) | uint32_t(slot); };
        std::unordered_map<int64_t, const FSItemStatList*> stats;
        for (int i = 0; i < c->ItemStatList.Num(); i++) {
            const FSItemStatList& s = c->ItemStatList[i];
            stats[key(int(s.ItemContainerType_13_189AD72B4DB6149046679DBC4B3F6AA9), s.ItemSlot_2_46BD91314F6FA6A67644D2AF115858C5)] = &s;
        }
        UClass* weapon = UBP_SpecItemWeapon_C::StaticClass();
        UClass* armor = UBP_SpecItemArmor_C::StaticClass();
        UClass* equipable = UBP_SpecItemEquipable_C::StaticClass();
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
            if (auto s = specs.find(it.specId); s != specs.end()) {
                UArchonSpec* sp = s->second;
                it.name = sp->GetName();
                if (sp->IsA(equipable)) it.equipSlot = int(static_cast<UBP_SpecItemEquipable_C*>(sp)->equipSlot);
                if (sp->IsA(weapon)) {
                    auto* w = static_cast<UBP_SpecItemWeapon_C*>(sp);
                    it.kind = Kind::Weapon;
                    it.attack = AttackOf(At(g_names.damageType, int(w->WeaponDamageType)), At(g_names.weaponType, int(w->WeaponAnimationType)));
                } else if (sp->IsA(armor)) it.kind = Kind::Armor;
            } else it.name = "spec " + std::to_string(it.specId);
            if (auto s = stats.find(key(it.containerType, it.slot)); s != stats.end()) {
                const auto& list = s->second->SingleStatList_20_0E6ABFCB406BAFE9EF5BF1A270AB3C00;
                for (int k = 0; k < list.Num(); k++)
                    it.stats.push_back({int(list[k].StatType_2_12180627459BAE0F2D872A9E86A4E38C), list[k].Value_5_2A2F09FE4A30C75D51D293AD5323C857});
            }
            out.push_back(std::move(it));
        }
    }

    // Memory reads only: render thread (menu) and game thread (sort request).
    // why: which link of the read path is missing (logged by the render thread when it changes).
    std::vector<Item> BuildModel(UBP_InvManagerComponent_C** invOut = nullptr, std::string* why = nullptr) {
        std::vector<Item> out;
        auto say = [&](const std::string& s) { if (why) *why = s; };
        UWorld* w = UWorld::GetWorld();
        APlayerController* any = nullptr;
        if (PtrOk(w) && PtrOk(w->OwningGameInstance) && w->OwningGameInstance->LocalPlayers.Num() > 0
            && PtrOk(w->OwningGameInstance->LocalPlayers[0]))
            any = w->OwningGameInstance->LocalPlayers[0]->PlayerController;
        ABP_PlayerControllerOnline_C* pc = LocalPC();
        if (!pc) { say(PtrOk(any) ? "local PC is " + any->Class->GetName() + ", not BP_PlayerControllerOnline_C" : "no local player controller"); return out; }
        UBP_InvManagerComponent_C* inv = PtrOk(pc->InvManagerComponent) ? pc->InvManagerComponent : nullptr;
        if (invOut) *invOut = inv;
        const auto specs = SpecMap();
        auto* c = Container(pc->InventoryItemContainerComponent);
        if (c) ReadContainer(c, false, specs, out);
        auto* b = inv ? BankOf(inv) : nullptr;
        if (b) ReadContainer(b, true, specs, out);
        say(std::string("pc ") + pc->Class->GetName() + (inv ? ", inv manager" : ", NO inv manager") + (c ? ", inventory " : ", NO inventory ")
            + std::to_string(c ? c->Items.Num() : 0) + (b ? ", bank " : ", NO bank ") + std::to_string(b ? b->Items.Num() : 0)
            + ", specs " + std::to_string(specs.size()));
        return out;
    }

    void LogModel(const std::vector<Item>& items) {
        std::map<std::pair<int, int>, int> perType;  // (bank, container type) → count
        int withStats = 0, named = 0, att[4] = {};
        for (const Item& it : items) {
            perType[{it.bank, it.containerType}]++;
            withStats += !it.stats.empty();
            named += !it.name.starts_with("spec ");
            if (it.kind == Kind::Weapon) att[int(it.attack)]++;
        }
        std::string s = "[item-sort] model " + std::to_string(items.size()) + " items:";
        for (auto& [k, n] : perType)
            s += std::string(" ") + (k.first ? "bank" : "inv") + "/type" + std::to_string(k.second) + "'" + At(g_names.container, k.second) + "'=" + std::to_string(n);
        s += " | stats " + std::to_string(withStats) + " spec " + std::to_string(named) + " | weapons melee " + std::to_string(att[1]) +
             " ranged " + std::to_string(att[2]) + " magic " + std::to_string(att[3]) + " unknown " + std::to_string(att[0]);
        logger::log(s);
    }

    // ---- game thread: Sort = the game's own slot list (its format), permuted by our order, into ReorderItems ----
    struct Request { bool storage; Profile profile; Filter filter; };
    SRWLOCK g_mu = SRWLOCK_INIT;  // not std::mutex (gotchas). Guards g_req, g_status, g_cands, g_anchors.
    Request g_req;                 // active profile + filter, refreshed by the render thread
    std::string g_status;
    std::atomic<bool> g_on{false};
    thread_local bool t_busy = false;  // our own UFunction calls re-enter ProcessEvent

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

    void RunSort(const Request& rq) {
        UBP_InvManagerComponent_C* inv = nullptr;
        std::vector<Item> all = BuildModel(&inv);
        if (!inv) return SetStatus("no inventory manager");
        UFunction* fnSort = inv->Class->GetFunction("BP_InvManagerComponent_C", "SortItemsInternalClient");
        UFunction* fnReorder = inv->Class->GetFunction("BP_InvManagerComponent_C", "ReorderItems");
        UFunction* fnDecode = UFItemContainerFunctions_C::StaticClass()->GetFunction("FItemContainerFunctions_C", "ConvertCompressedItemSlot");
        if (!fnSort || !fnReorder || !fnDecode) return SetStatus("game functions not found");

        std::vector<Item> items;  // candidates: the container being sorted
        for (Item& it : all) if (it.bank == rq.storage) items.push_back(std::move(it));
        std::vector<float> score;
        for (const Item& it : items) score.push_back(Score(it, rq.profile, g_names.stat));

        // ponytail: the out TArray is allocated by the game and leaked (a few hundred bytes per press)
        Params::BP_InvManagerComponent_C_SortItemsInternalClient sp{};
        sp.SortItem = EItemSort(0);
        sp.IsStorage = rq.storage;
        inv->ProcessEvent(fnSort, &sp);
        const std::vector<int> vals = ReadInts(sp.SlotsToMove);

        std::vector<int> itemOf(vals.size(), -1);
        const int plain = PlainType(vals, items);
        for (int i = 0; i < int(vals.size()); i++) {
            int slot = vals[i], type = plain;
            if (plain < 0) {
                Params::FItemContainerFunctions_C_ConvertCompressedItemSlot dp{};
                dp.CompressedItemSlot = vals[i];
                dp.__WorldContext = inv;
                UFItemContainerFunctions_C::GetDefaultObj()->ProcessEvent(fnDecode, &dp);
                slot = dp.ItemSlot;
                type = int(dp.ContainerType);
            }
            for (int k = 0; k < int(items.size()); k++)
                if (items[k].slot == slot && items[k].containerType == type) { itemOf[i] = k; break; }
        }
        const std::vector<int> order = Reorder(vals, itemOf, items, score, rq.filter);
        char head[160];
        std::snprintf(head, sizeof(head), "%s: game list %d entries (%s), %d items in container", rq.storage ? "bank" : "inventory",
                      int(vals.size()), plain >= 0 ? "plain slots" : "encoded slots", int(items.size()));
        if (order.empty()) {
            std::string dump;
            for (int i = 0; i < int(vals.size()) && i < 40; i++) dump += " " + std::to_string(vals[i]) + (itemOf[i] < 0 ? "?" : "");
            return SetStatus(std::string(head) + " - not applied (unmatched entries:" + dump + ")");
        }
        Params::BP_InvManagerComponent_C_ReorderItems rp{};
        RawArray arr{const_cast<int*>(order.data()), int32(order.size()), int32(order.size())};
        std::memcpy(&rp.SlotsToMove, &arr, sizeof(arr));
        rp.IsStorage = rq.storage;
        inv->ProcessEvent(fnReorder, &rp);
        std::memset(&rp.SlotsToMove, 0, sizeof(arr));  // our memory, not the game's
        SetStatus(std::string(head) + " - reordered by profile '" + rq.profile.name + "'");
    }

    // ---- game thread: hooked vanilla sort, bag header geometry ----
    struct Cand { UWidgetitemBagHeaderMenu_C* w; int32 idx; };
    struct Anchor { Rect r; bool storage; };
    std::vector<Cand> g_cands;      // live bag headers (render thread scans GObjects)
    std::vector<Anchor> g_anchors;  // visible ones, viewport pixels (game thread)
    std::unordered_map<UFunction*, std::string> g_triggers;  // read-only once g_on
    UFunction *g_fnGeom = nullptr, *g_fnLocalSize = nullptr, *g_fnToViewport = nullptr;
    unsigned g_pendingMask = 0;  // game thread: 1 inventory, 2 bank
    ULONGLONG g_due = 0, g_nextGeom = 0;

    // Same call shape as Dumper-7's generated bodies for native functions.
    void CallNative(const UObject* obj, UFunction* fn, void* parms) {
        auto flags = fn->FunctionFlags;
        fn->FunctionFlags |= 0x400;  // FUNC_Native
        obj->ProcessEvent(fn, parms);
        fn->FunctionFlags = flags;
    }

    std::string ClassName(UObject* o) { return PtrOk(o) && PtrOk(o->Class) ? o->Class->GetName() : ""; }
    bool HeaderIsStorage(UWidgetitemBagHeaderMenu_C* h) { return StorageWidget(ClassName(h->Owning_Widget)); }

    // Visible = no Collapsed/Hidden widget and no inactive switcher page up the chain, across nested user widgets.
    bool Shown(UWidget* w) {
        for (int depth = 0; PtrOk(w) && depth < 64; depth++) {
            if (w->Visibility == ESlateVisibility::Collapsed || w->Visibility == ESlateVisibility::Hidden) return false;
            UPanelSlot* s = w->Slot;
            if (PtrOk(s) && PtrOk(s->Parent)) {
                UPanelWidget* p = s->Parent;
                if (p->IsA(UWidgetSwitcher::StaticClass())) {
                    const int i = static_cast<UWidgetSwitcher*>(p)->ActiveWidgetIndex;
                    if (i < 0 || i >= p->Slots.Num() || p->Slots[i] != s) return false;
                }
                w = p;
                continue;
            }
            UObject* tree = w->Outer;  // root of a user widget's tree: continue with the user widget that owns it
            if (PtrOk(tree) && tree->IsA(UWidgetTree::StaticClass()) && PtrOk(tree->Outer) && tree->Outer->IsA(UWidget::StaticClass())) {
                w = static_cast<UWidget*>(tree->Outer);
                continue;
            }
            return true;
        }
        return false;
    }

    bool RectOf(UWidget* w, Rect& r) {
        Params::Widget_GetCachedGeometry g{};
        CallNative(w, g_fnGeom, &g);
        Params::SlateBlueprintLibrary_GetLocalSize ls{};
        ls.Geometry = g.ReturnValue;
        CallNative(USlateBlueprintLibrary::GetDefaultObj(), g_fnLocalSize, &ls);
        FVector2D px[2];
        const FVector2D corner[2] = {{0, 0}, ls.ReturnValue};
        for (int i = 0; i < 2; i++) {
            Params::SlateBlueprintLibrary_LocalToViewport v{};
            v.WorldContextObject = w;
            v.Geometry = g.ReturnValue;
            v.LocalCoordinate = corner[i];
            CallNative(USlateBlueprintLibrary::GetDefaultObj(), g_fnToViewport, &v);
            px[i] = v.PixelPosition;
        }
        r = {px[0].X, px[0].Y, px[1].X - px[0].X, px[1].Y - px[0].Y};
        return r.w > 1 && r.h > 1;
    }

    void UpdateAnchors() {
        std::vector<Cand> cands;
        AcquireSRWLockShared(&g_mu); cands = g_cands; ReleaseSRWLockShared(&g_mu);
        std::vector<Anchor> out;
        ABP_PlayerControllerOnline_C* pc = LocalPC();
        if (pc && pc->bShowMouseCursor)  // inventory open = game cursor shown
            for (const Cand& c : cands) {
                if (UObject::GObjects->GetByIndex(c.idx) != c.w || !Shown(c.w)) continue;  // freed or hidden
                Rect r;
                if (RectOf(c.w, r)) out.push_back({r, HeaderIsStorage(c.w)});
            }
        AcquireSRWLockExclusive(&g_mu); g_anchors = std::move(out); ReleaseSRWLockExclusive(&g_mu);
    }

    void OnEvent(void* objp, void* fnp, void* parms) {
        if (t_busy || !g_on.load(std::memory_order_relaxed)) return;
        const ULONGLONG now = GetTickCount64();
        t_busy = true;
        if (auto it = g_triggers.find(static_cast<UFunction*>(fnp)); it != g_triggers.end()) {
            auto* obj = static_cast<UObject*>(objp);
            const int off = StorageParamOffset(it->second);
            bool storage = false;
            if (off >= 0 && parms) storage = static_cast<const bool*>(parms)[off];
            else if (PtrOk(obj) && obj->IsA(UWidgetitemBagHeaderMenu_C::StaticClass())) storage = HeaderIsStorage(static_cast<UWidgetitemBagHeaderMenu_C*>(obj));
            g_pendingMask |= storage ? 2 : 1;
            g_due = now + kSettleMs;
            static std::unordered_map<UFunction*, int> logged;
            if (!logged[static_cast<UFunction*>(fnp)]++)
                logger::log("[item-sort] vanilla sort seen: " + it->second + " on " + ClassName(obj) + (storage ? " (bank)" : " (inventory)"));
        }
        if (g_pendingMask && now >= g_due) {
            AcquireSRWLockShared(&g_mu);
            Request rq = g_req;
            ReleaseSRWLockShared(&g_mu);
            for (int b = 0; b < 2; b++)
                if (g_pendingMask & (1u << b)) { rq.storage = b == 1; RunSort(rq); }
            g_pendingMask = 0;
        }
        if (now >= g_nextGeom) { g_nextGeom = now + 100; UpdateAnchors(); }
        t_busy = false;
    }

    // Render thread, once: functions the listener needs. Children = functions declared on that class.
    bool Resolve() {
        g_fnGeom = UWidget::StaticClass()->GetFunction("Widget", "GetCachedGeometry");
        g_fnLocalSize = USlateBlueprintLibrary::StaticClass()->GetFunction("SlateBlueprintLibrary", "GetLocalSize");
        g_fnToViewport = USlateBlueprintLibrary::StaticClass()->GetFunction("SlateBlueprintLibrary", "LocalToViewport");
        UClass* classes[] = {UWidgetitemBagHeaderMenu_C::StaticClass(), UBP_HUDInventoryComponent_C::StaticClass(),
                             UBP_InvManagerComponent_C::StaticClass(), ABP_PlayerControllerGame_C::StaticClass(),
                             UWidgetItemInventory_C::StaticClass(), UWidgetItemBag_C::StaticClass()};
        std::string names;
        for (UClass* c : classes) {
            if (!PtrOk(c)) return false;
            for (UField* f = c->Children; PtrOk(f); f = f->Next) {
                const std::string n = f->GetName();
                if (f->IsA(UFunction::StaticClass()) && SortTrigger(n)) { g_triggers[static_cast<UFunction*>(f)] = n; names += " " + n; }
            }
        }
        logger::log("[item-sort] sort triggers:" + names);
        return g_fnGeom && g_fnLocalSize && g_fnToViewport && !g_triggers.empty();
    }

    struct ItemSort : feature::Feature {
        std::vector<Profile> profiles = Presets();
        int cur = 0;
        Filter filter;
        char search[64] = {};
        std::vector<Item> items;  // for the weights popup (stats present on the player's items)
        bool resolved = false;
        double nextScan = 0, nextModel = 0;
        std::string lastWhy;
        ImVec2 lastSize{0, 0};

        ItemSort() : Feature("Item sort", feature::Stage::Alpha) { optIn = true; }  // new game-thread hook

        void Off() override {
            g_on = false;
            game::SetEventListener(&OnEvent, false);
            AcquireSRWLockExclusive(&g_mu); g_anchors.clear(); ReleaseSRWLockExclusive(&g_mu);
        }

        Profile* Find(const std::string& name) {
            for (Profile& p : profiles) if (p.name == name) return &p;
            return nullptr;
        }
        // profile=<i> | w.<profile>.<stat>=<weight> | a.<profile>.<attack>=<weight>
        void Load(const char* key, const char* value) override {
            const std::string k = key;
            if (k == "profile") { cur = std::clamp(std::atoi(value), 0, int(profiles.size()) - 1); return; }
            const size_t dot = k.find('.', 2);
            if (k.size() < 3 || k[1] != '.' || dot == std::string::npos) return;
            Profile* p = Find(k.substr(2, dot - 2));
            if (!p) return;
            const float v = std::clamp(float(std::atof(value)), 0.0f, 2.0f);
            if (k[0] == 'w') p->weight[k.substr(dot + 1)] = v;
            if (k[0] == 'a') p->attack[std::atoi(k.c_str() + dot + 1)] = v;
        }
        void Save(std::vector<std::pair<std::string, std::string>>& out) override {
            out.push_back({"profile", std::to_string(cur)});
            for (const Profile& p : profiles) {
                for (auto& [s, w] : p.weight) out.push_back({"w." + p.name + "." + s, std::to_string(w)});
                for (auto& [a, w] : p.attack) out.push_back({"a." + p.name + "." + std::to_string(a), std::to_string(w)});
            }
        }

        void LoadNames() {
            Names n{EnumNames("EStatType"), EnumNames("EItemContainerType"), EnumNames("EWeaponType"), EnumNames("EWeaponDamageType")};
            std::string s = "[item-sort] enums: stats " + std::to_string(n.stat.size()) + ", containers";
            for (size_t i = 0; i < n.container.size(); i++) s += " " + std::to_string(i) + "=" + n.container[i];
            s += ", damage types";
            for (size_t i = 0; i < n.damageType.size(); i++) s += " " + std::to_string(i) + "=" + n.damageType[i];
            s += ", weapon types";
            for (size_t i = 0; i < n.weaponType.size(); i++) s += " " + std::to_string(i) + "=" + n.weaponType[i];
            logger::log(s);
            if (n.stat.empty() || n.container.empty()) return;  // retried next scan
            g_names = std::move(n);
            g_haveNames.store(true, std::memory_order_release);
        }

        // Every second: enum names (once), bag header widgets, model diagnostics.
        void Scan() {
            if (!g_haveNames.load(std::memory_order_acquire)) LoadNames();
            std::vector<Cand> cands;
            UClass* cls = UWidgetitemBagHeaderMenu_C::StaticClass();
            for (int i = 0; PtrOk(cls) && i < UObject::GObjects->Num(); i++) {
                UObject* o = UObject::GObjects->GetByIndex(i);
                if (PtrOk(o) && o->IsA(cls) && !o->IsDefaultObject()) cands.push_back({static_cast<UWidgetitemBagHeaderMenu_C*>(o), o->Index});
            }
            AcquireSRWLockExclusive(&g_mu);
            g_cands = std::move(cands);
            g_req.profile = profiles[cur];
            g_req.filter = filter;
            ReleaseSRWLockExclusive(&g_mu);
        }

        void RefreshModel() {
            std::string why;
            items = BuildModel(nullptr, &why);
            if (why != lastWhy) { logger::log("[item-sort] read: " + why); lastWhy = why; LogModel(items); }
        }

        void Publish() {
            AcquireSRWLockExclusive(&g_mu);
            g_req.profile = profiles[cur];
            g_req.filter = filter;
            ReleaseSRWLockExclusive(&g_mu);
            ImGui::MarkIniSettingsDirty();
        }

        void Weights(Profile& p) {
            bool dirty = false;
            for (int a = 1; a < 4; a++) {
                float v = AttackWeight(p, Attack(a));
                if (ImGui::SliderFloat((std::string(kAttackName[a]) + " weapons").c_str(), &v, 0, 2, "%.2f")) { p.attack[a] = v; dirty = true; }
            }
            std::vector<std::string> seen;
            for (const Item& it : items)
                for (const Stat& s : it.stats) seen.push_back(StatName(g_names.stat, s.type));
            std::sort(seen.begin(), seen.end());
            seen.erase(std::unique(seen.begin(), seen.end()), seen.end());
            for (const std::string& s : seen) {
                float v = Weight(p, s);
                if (ImGui::SliderFloat(s.c_str(), &v, 0, 2, "%.2f")) { p.weight[s] = v; dirty = true; }
            }
            if (seen.empty()) ImGui::TextDisabled("no item stats read yet");
            if (ImGui::Button("Reset profile")) { p.weight.clear(); p.attack.clear(); dirty = true; }
            if (dirty) Publish();
        }

        // Design spec: design-system.md "Inventory sort strip" (menu/HUD panel tokens, display font kSm).
        void Strip(const feature::Frame& f, const Anchor& a) {
            using namespace style;
            const float ui = type::Ui(f.h);
            const Rect r = PanelRect(a.r, lastSize.x, lastSize.y, kGapToHeader * ui, f.w, f.h);
            auto col = [](Rgba c, float alpha = 1.0f) { return ImGui::ColorConvertU32ToFloat4(Pack(c, alpha)); };
            const std::pair<ImGuiCol, ImVec4> colors[] = {
                {ImGuiCol_WindowBg, col(color::kPanel)}, {ImGuiCol_PopupBg, col(color::kPanel)}, {ImGuiCol_Border, col(color::kPanelEdge)},
                {ImGuiCol_Text, col(color::kText)}, {ImGuiCol_TextDisabled, col(color::kTextMuted)},
                {ImGuiCol_CheckMark, col(color::kGold)}, {ImGuiCol_SliderGrab, col(color::kGold)}, {ImGuiCol_SliderGrabActive, col(color::kGold)},
                {ImGuiCol_FrameBg, col(color::kGold, .25f)}, {ImGuiCol_FrameBgHovered, col(color::kGold, .40f)}, {ImGuiCol_FrameBgActive, col(color::kGold, .55f)},
                {ImGuiCol_Button, col(color::kGold, .25f)}, {ImGuiCol_ButtonHovered, col(color::kGold, .40f)}, {ImGuiCol_ButtonActive, col(color::kGold, .55f)},
                {ImGuiCol_Header, col(color::kGold, .25f)}, {ImGuiCol_HeaderHovered, col(color::kGold, .40f)}, {ImGuiCol_HeaderActive, col(color::kGold, .55f)}};
            for (auto& [k, v] : colors) ImGui::PushStyleColor(k, v);
            ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, radius::kLg * ui);
            ImGui::PushStyleVar(ImGuiStyleVar_PopupRounding, radius::kLg * ui);
            ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, radius::kMd * ui);
            ImGui::PushStyleVar(ImGuiStyleVar_GrabRounding, radius::kMd * ui);
            ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(space::k6 * ui, space::k6 * ui));
            ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(space::k4 * ui, space::k2 * ui));
            ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(space::k4 * ui, space::k3 * ui));
            ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 1.0f);
            const float oldScale = f.font->Scale;
            f.font->Scale = type::kSm * ui / type::kAtlasPx;
            ImGui::PushFont(f.font);

            ImGui::SetNextWindowPos(ImVec2(r.x, r.y));
            const ImGuiWindowFlags wf = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings |
                                        ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoMove;
            if (ImGui::Begin("##item-sort-strip", nullptr, wf)) {
                Profile& p = profiles[cur];
                ImGui::AlignTextToFramePadding();
                ImGui::TextDisabled("Sort by");
                ImGui::SameLine();
                ImGui::SetNextItemWidth(kComboW * ui);
                if (ImGui::BeginCombo("##profile", p.name.c_str())) {
                    for (int i = 0; i < int(profiles.size()); i++)
                        if (ImGui::Selectable(profiles[i].name.c_str(), i == cur)) { cur = i; Publish(); }
                    ImGui::EndCombo();
                }
                ImGui::SameLine();
                if (ImGui::Button("Weights")) ImGui::OpenPopup("##weights");
                if (ImGui::BeginPopup("##weights")) { Weights(profiles[cur]); ImGui::EndPopup(); }
                ImGui::SameLine();
                ImGui::TextDisabled("First");
                ImGui::SameLine();
                static const char* const kKinds[] = {"Any", "Other", "Weapons", "Armor"};
                static const char* const kAttacks[] = {"Any", "-", "Melee", "Ranged", "Magic"};
                ImGui::SetNextItemWidth(kFilterW * ui);
                int k = filter.kind + 1;
                if (ImGui::Combo("##kind", &k, kKinds, IM_ARRAYSIZE(kKinds))) { filter.kind = k - 1; Publish(); }
                ImGui::SameLine();
                ImGui::SetNextItemWidth(kFilterW * ui);
                int at = filter.attack + 1;
                if (ImGui::Combo("##attack", &at, kAttacks, IM_ARRAYSIZE(kAttacks))) { filter.attack = at - 1; Publish(); }
                ImGui::SameLine();
                ImGui::SetNextItemWidth(kSearchW * ui);
                if (ImGui::InputTextWithHint("##search", "name", search, sizeof(search))) { filter.text = search; Publish(); }
                AcquireSRWLockShared(&g_mu);
                const std::string st = g_status;
                ReleaseSRWLockShared(&g_mu);
                if (st.find("not applied") != std::string::npos || st.find("not found") != std::string::npos || st.find("no inv") != std::string::npos)
                    ImGui::TextDisabled("%s", st.c_str());
                lastSize = ImGui::GetWindowSize();
            }
            ImGui::End();
            ImGui::PopFont();
            f.font->Scale = oldScale;
            ImGui::PopStyleVar(8);
            ImGui::PopStyleColor(IM_ARRAYSIZE(colors));
        }

        // Render thread: memory reads only; UFunction work happens in OnEvent (game thread).
        void OnFrame(const feature::Frame& f) override {
            if (f.now >= nextScan) {
                nextScan = f.now + 1.0;
                Scan();
                if (!resolved && (resolved = Resolve())) { g_on = true; game::SetEventListener(&OnEvent, true); }
            }
            AcquireSRWLockShared(&g_mu);
            const std::vector<Anchor> anchors = g_anchors;
            ReleaseSRWLockShared(&g_mu);
            if (anchors.empty() || !g_haveNames.load(std::memory_order_acquire)) return;
            if (f.now >= nextModel) { nextModel = f.now + 1.0; RefreshModel(); }
            const Anchor* a = &anchors[0];  // one strip: the inventory bag when the bank shows both
            for (const Anchor& x : anchors) if (!x.storage) { a = &x; break; }
            Strip(f, *a);
            feature::wantInput = true;
        }
    } g_item_sort;
}

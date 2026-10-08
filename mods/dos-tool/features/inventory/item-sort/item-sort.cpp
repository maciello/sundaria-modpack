#include "feature.hpp"
#include "item-sort.hpp"
#include "inventory-ui.hpp"
#include "logger.hpp"
#include "cost.hpp"
#include "game.hpp"
#include "umg.hpp"
#include "ref.hpp"
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

// Item sort (non-visual part; the inventory UI is the designer's): the game's own Sort button sorts the bag
// by the active profile (item-sort.hpp: score, order). Game thread (ProcessEvent listener): read items +
// stats, compute our order, apply it through the game's reorder UFunction, read the container back.
// Render thread: enum/attribute names, bank lookup, dev triggers. API for the UI: item_sort::api (item-sort.hpp).
// Dev loop: files next to the exe trigger work once: item-sort.probe (log only), item-sort.profile<i>, item-sort.apply / item-sort.apply-bank.
using namespace SDK;

namespace {
    using namespace item_sort;
    namespace io = items::io;
    std::string At(const std::vector<std::string>& v, int i) { return i >= 0 && i < int(v.size()) ? v[i] : ""; }

    inline bool PtrOk(const void* p) {
        const uintptr_t v = reinterpret_cast<uintptr_t>(p);
        return v > 0x10000 && v < 0x7FFFFFFFFFFFull;
    }
    struct RawArray { void* data; int32 num, max; };  // TArray layout
    std::string I(long long v) { return std::to_string(v); }

    SRWLOCK g_mu = SRWLOCK_INIT;  // not std::mutex (gotchas). Guards g_status.

    // ---- game thread ----
    thread_local bool t_busy = false;  // our own UFunction calls re-enter ProcessEvent
    std::string g_status;
    std::atomic<unsigned> g_request{0};           // 1 inventory, 2 bank (API, dev files)
    std::atomic<bool> g_probe{false}, g_on{false}, g_verbose{false};  // verbose: per-item sort dump, set by item-sort.probe
    // Functions that start the game's own sort, read from three Blueprint classes that come and go with the map (#63).
    ref::Cached<UClass> g_trigCls[3] = {{[] { return UWidgetitemBagHeaderMenu_C::StaticClass(); }},
                                        {[] { return UBP_HUDInventoryComponent_C::StaticClass(); }},
                                        {[] { return UBP_InvManagerComponent_C::StaticClass(); }}};
    ref::Ref g_trigFrom[3];  // the classes g_triggers was read from; their functions die with them
    std::unordered_map<ref::Ref, std::string, ref::Hash> g_triggers;  // UFunction -> name
    unsigned g_pendingMask = 0;  // Sort pressed: 1 inventory, 2 bank
    ref::Ref g_button;  // UFunction: the header's Sort click; the game's own sort is replaced by ours

    struct Where2 { UObject* pc; UBP_InvManagerComponent_C* inv; UBP_ItemContainerComponent_C* bag; UBP_ItemContainerComponent_C* bank; std::string how; };
    Where2 Locate() {
        const io::Located l = io::Locate();
        return {static_cast<UObject*>(l.pc), static_cast<UBP_InvManagerComponent_C*>(l.inv), static_cast<UBP_ItemContainerComponent_C*>(l.bag),
                static_cast<UBP_ItemContainerComponent_C*>(l.bank), l.how};
    }

    void SetStatus(const std::string& s) {
        AcquireSRWLockExclusive(&g_mu); g_status = s; ReleaseSRWLockExclusive(&g_mu);
        logger::log("[item-sort] " + s);
    }
    UFunction* Fn(UObject* o, const char* cls, const char* name) { return PtrOk(o) ? o->Class->GetFunction(cls, name) : nullptr; }

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
        for (auto& [t, n] : perType) s += " type" + I(t) + "'" + At(io::GetNames().container, t) + "'=" + I(n);
        s += " | weapons " + I(kinds[1]) + " armor " + I(kinds[2]) + " other " + I(kinds[0]) + " | with stats " + I(withStats) +
             " | melee " + I(att[1]) + " ranged " + I(att[2]) + " magic " + I(att[3]) + " unknown " + I(att[0]);
        logger::log(s);
        for (const Item& it : items)
            if (!it.stats.empty()) {
                std::string st;
                for (const Stat& x : it.stats) st += " " + StatName(io::GetNames().stat, x.type) + "=" + std::to_string(x.value).substr(0, 6);
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

    // Order of the bag right now: item keys by ascending slot; slots: those slots.
    std::vector<long long> CurrentOrder(UBP_ItemContainerComponent_C* c, bool bank, std::vector<int>& slots) {
        std::vector<Item> items = Bag(io::Read(c, bank, false));
        std::sort(items.begin(), items.end(), [](const Item& a, const Item& b) { return a.slot < b.slot; });
        std::vector<long long> keys;
        for (const Item& it : items) { keys.push_back(KeyOf(it)); slots.push_back(it.slot); }
        return keys;
    }

    void CallWithArray(UObject* obj, UFunction* fn, void* parms, TArray<int32>& field, const std::vector<int>& v) {
        RawArray arr{const_cast<int*>(v.data()), int32(v.size()), int32(v.size())};
        std::memcpy(&field, &arr, sizeof(arr));
        obj->ProcessEvent(fn, parms);
        std::memset(&field, 0, sizeof(arr));  // our memory, not the game's
    }

    double Ms(LARGE_INTEGER& t) {  // ms since t; t = now
        LARGE_INTEGER n, f;
        QueryPerformanceCounter(&n);
        QueryPerformanceFrequency(&f);
        const double ms = double(n.QuadPart - t.QuadPart) * 1000.0 / double(f.QuadPart);
        t = n;
        return ms;
    }
    std::string Ms2(double ms) { return std::to_string(ms).substr(0, 5); }

    // Sort one bag by the active profile through InvManager.ReorderItems (verified path), proven by one readback.
    // One log line; the per-item dump only after item-sort.probe (g_verbose).
    void RunSort(bool bank) {  // O(items in the container), once per Sort press
        static cost::Path path{"item-sort apply"};
        cost::Scope cs(path);
        const char* what = bank ? "bank" : "inventory";
        LARGE_INTEGER t;
        QueryPerformanceCounter(&t);
        Where2 w = Locate();
        UBP_ItemContainerComponent_C* c = bank ? w.bank : w.bag;
        UFunction* fn = Fn(w.inv, "BP_InvManagerComponent_C", "ReorderItems");
        if (!c || !fn) return SetStatus(std::string(what) + ": container not found (" + w.how + ")");
        const Profile prof = items::profiles::Active();

        std::vector<Item> items = Bag(io::Read(c, bank, true));
        const double readMs = Ms(t);
        if (items.size() < 2) return SetStatus(std::string(what) + ": nothing to sort");
        std::vector<float> score;
        const std::vector<int> idx = Order(items, prof, io::GetNames().stat, &score);
        std::vector<long long> intended;
        std::vector<int> bySlots;
        for (int i : idx) { intended.push_back(KeyOf(items[i])); bySlots.push_back(items[i].slot); }
        bySlots.resize(PrefixToMove(bySlots));  // the game re-adds every listed item (~0.4 ms each): list only what moves
        const double orderMs = Ms(t);

        if (!bySlots.empty()) {
            Params::BP_InvManagerComponent_C_ReorderItems p{};
            p.IsStorage = bank;
            CallWithArray(w.inv, fn, &p, p.SlotsToMove, bySlots);
        }
        const double applyMs = Ms(t);
        std::vector<int> after;
        const int ok = InOrder(intended, CurrentOrder(c, bank, after));
        const int holes = Holes(after);
        const double checkMs = Ms(t);
        SetStatus(std::string(what) + (ok == int(intended.size()) && !holes ? ": sorted by '" : ": NOT in order after ReorderItems, profile '") + prof.name +
                  "' " + I(ok) + "/" + I(intended.size()) + ", " + I(bySlots.size()) + " sent, holes " + I(holes) + " | ms read " + Ms2(readMs) + " order " + Ms2(orderMs) + " apply " + Ms2(applyMs) +
                  " check " + Ms2(checkMs) + " total " + Ms2(readMs + orderMs + applyMs + checkMs));
        if (!g_verbose) return;
        LogItems(what, items);
        for (int k = 0; k < int(idx.size()); k++) {
            const Item& it = items[idx[k]];
            const Bucket bk = BucketOf(it, prof.focus);
            const std::string sub = bk.group == 0 ? it.typeName : bk.group == 3 ? "" : At(io::GetNames().equipSlot, it.equipSlot) + "#" + I(it.equipSlot);
            logger::log("[item-sort]   " + I(k + 1) + ". " + kKindName[int(it.kind)] + (bk.group == 2 ? "(equipable)" : "") + "/" + sub + " '" +
                        it.name + "' " + std::to_string(score[idx[k]]).substr(0, 5) + " lv" + I(it.level));
        }
    }

    void Probe() {
        Where2 w = Locate();
        logger::log("[item-sort] probe containers with items:" + io::ContainersReport());
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
                LogItems(c == w.bag ? "probe bag" : "probe bank", io::Read(c, c == w.bank, true));
        }
    }

    // Game thread, per event: O(1) while the three classes live; re-read (O(their functions)) once per class load.
    void Triggers() {
        bool same = true;
        for (int i = 0; i < 3; i++) { g_trigCls[i].Get(); same &= g_trigFrom[i] == g_trigCls[i].r; }
        if (same) return;
        g_triggers.clear();
        g_button = {};
        std::string names;
        for (int i = 0; i < 3; i++) {
            g_trigFrom[i] = g_trigCls[i].r;
            UClass* c = g_trigCls[i].Get();
            for (UField* f = c ? c->Children : nullptr; PtrOk(f); f = f->Next) {
                const std::string n = f->GetName();
                if (f->IsA(UFunction::StaticClass()) && SortTrigger(n)) {
                    g_triggers[ref::Ref(f)] = n;
                    names += " " + n;
                    if (n.starts_with("BndEvt__Button_Sort")) g_button = ref::Ref(f);
                }
            }
        }
        if (!names.empty()) logger::log("[item-sort] sort triggers:" + names);
    }

    void OnEvent(void* objp, void* fnp, void* parms) {
        if (t_busy || !g_on.load(std::memory_order_relaxed) || !game::OnGameThread()) return;  // shared state, UFunction calls and ref resolution: game thread only
        Triggers();
        if (auto it = g_triggers.find(ref::Ref(fnp)); it != g_triggers.end()) {
            auto* obj = static_cast<UObject*>(objp);
            const int off = StorageParamOffset(it->second);
            bool storage = false;
            if (off >= 0 && parms) storage = static_cast<const bool*>(parms)[off];
            else if (PtrOk(obj) && obj->IsA(UWidgetitemBagHeaderMenu_C::StaticClass())) storage = static_cast<UWidgetitemBagHeaderMenu_C*>(obj)->IsStorage;
            g_pendingMask |= storage ? 2 : 1;
            logger::log("[item-sort] Sort pressed: " + it->second + (storage ? " (bank)" : " (inventory)"));
        }
        if (!umg::IsWorldTick(fnp)) return;  // reorders refresh the bag's widgets: world tick only (#50)
        const bool probe = g_probe.exchange(false);
        const unsigned req = g_request.exchange(0);
        const unsigned todo = req | g_pendingMask;
        if (!probe && !todo) return;
        t_busy = true;
        if (probe) Probe();
        for (int b = 0; b < 2; b++)
            if (todo & (1u << b)) RunSort(b == 1);
        g_pendingMask = 0;
        t_busy = false;
    }


    // The header's Sort click: skip the game's sort (it re-adds every item, ~35 ms + a spread second pass), ours runs once.
    bool SkipVanillaSort(void*, void* fn, void*) { return g_on.load(std::memory_order_relaxed) && g_button.Is(fn); }

    struct ItemSort : feature::Feature {
        double nextScan = 0;
        int savedActive = -1;
        std::string exeDir;

        ItemSort() : Feature("Item sort", feature::Stage::Beta) {}

        void Off() override {
            g_on = false;
            game::SetEventFilter(&SkipVanillaSort, false);
            game::SetEventListener(&OnEvent, false);
            item_sort::ui::Off();
        }

        // profile=<i> | w.<profile>.<stat>=<weight> | a.<profile>.<attack>=<weight>
        void Load(const char* key, const char* value) override {
            const std::string k = key;
            if (k == "profile") items::profiles::SetActive(std::atoi(value));
            std::vector<Profile> all = items::profiles::All();
            const size_t dot = k.find('.', 2);
            if (k.size() >= 3 && k[1] == '.' && dot != std::string::npos)
                for (Profile& p : all) {
                    if (p.name != k.substr(2, dot - 2)) continue;
                    const float v = std::clamp(float(std::atof(value)), 0.0f, 2.0f);
                    if (k[0] == 'w') p.weight[k.substr(dot + 1)] = v;
                    if (k[0] == 'a') p.attack[std::atoi(k.c_str() + dot + 1)] = v;
                }
            items::profiles::SetAll(std::move(all));
        }
        void Save(std::vector<std::pair<std::string, std::string>>& out) override {
            out.push_back({"profile", I(items::profiles::ActiveIndex())});
            for (const Profile& p : items::profiles::All()) {
                for (auto& [s, w] : p.weight) out.push_back({"w." + p.name + "." + s, std::to_string(w)});
                for (auto& [a, w] : p.attack) out.push_back({"a." + p.name + "." + I(a), std::to_string(w)});
            }
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
            if (const int a = items::profiles::ActiveIndex(); savedActive != a) { if (savedActive >= 0) ImGui::MarkIniSettingsDirty(); savedActive = a; }
            if (f.now < nextScan) return;
            nextScan = f.now + 1.0;
            if (exeDir.empty()) {
                char buf[MAX_PATH] = {};
                GetModuleFileNameA(nullptr, buf, MAX_PATH);
                exeDir = buf;
                exeDir = exeDir.substr(0, exeDir.find_last_of("\\/") + 1);
            }
            io::Tick();
            if (!io::Ready()) return;
            if (!g_on) { g_on = true; game::SetEventListener(&OnEvent, true); game::SetEventFilter(&SkipVanillaSort, true); }
            item_sort::ui::Frame();  // game buttons in the inventory/bank header (inventory-ui.cpp)

            if (TakeFile("item-sort.probe")) g_probe = g_verbose = true;
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
        for (const Profile& p : items::profiles::All()) out.push_back(p.name);
        return out;
    }
    int ActiveProfile() { return items::profiles::ActiveIndex(); }
    void SetActiveProfile(int i) { items::profiles::SetActive(i); }
    void RequestSort(bool bank) { g_request |= bank ? 2u : 1u; }
    std::string LastStatus() {
        AcquireSRWLockShared(&g_mu);
        std::string s = g_status;
        ReleaseSRWLockShared(&g_mu);
        return s;
    }
}

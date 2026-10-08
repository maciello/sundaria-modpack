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
#include "FItemContainerFunctions_classes.hpp"
#include "FItemContainerFunctions_parameters.hpp"

// Item sort: every item in inventory, bank and equipment as one model (item-sort.hpp), scored by a profile of
// stat weights; filtered list in the Insert menu, and "Sort" reorders the real inventory/bank through the
// game's own ReorderItems (game thread). Model = memory reads only (render thread).
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
        UEnum* e = UObject::FindObjectFast<UEnum>(name);
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

    // Written once on the render thread before any game-thread request (published by the request lock).
    struct Names { std::vector<std::string> stat, container, weaponType, damageType; } g_names;
    bool g_haveNames = false;

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
    std::vector<Item> BuildModel(UBP_InvManagerComponent_C** invOut = nullptr) {
        std::vector<Item> out;
        ABP_PlayerControllerOnline_C* pc = LocalPC();
        if (!pc) return out;
        UBP_InvManagerComponent_C* inv = PtrOk(pc->InvManagerComponent) ? pc->InvManagerComponent : nullptr;
        if (invOut) *invOut = inv;
        const auto specs = SpecMap();
        if (auto* c = Container(pc->InventoryItemContainerComponent)) ReadContainer(c, false, specs, out);
        if (inv)
            if (auto* b = BankOf(inv)) ReadContainer(b, true, specs, out);
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
    struct Request { bool storage; Profile profile; };
    SRWLOCK g_mu = SRWLOCK_INIT;  // not std::mutex (gotchas)
    Request g_req;
    std::string g_status;
    std::atomic<bool> g_pending{false}, g_on{false};
    thread_local bool t_busy = false;

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
        const std::vector<int> order = Reorder(vals, itemOf, items, score);
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

    void OnEvent(void*, void*, void*) {
        if (t_busy || !g_on.load(std::memory_order_relaxed) || !g_pending.exchange(false)) return;
        t_busy = true;
        AcquireSRWLockShared(&g_mu);
        const Request rq = g_req;
        ReleaseSRWLockShared(&g_mu);
        RunSort(rq);
        t_busy = false;
    }

    struct ItemSort : feature::Feature {
        std::vector<Profile> profiles = Presets();
        int cur = 0;
        Filter filter;
        char search[64] = {};
        std::vector<Item> items;
        std::vector<float> score;
        std::vector<int> order;
        double nextBuild = 0;
        size_t loggedCount = SIZE_MAX;

        ItemSort() : Feature("Item sort", feature::Stage::Alpha) {}

        void Off() override {
            g_on = false;
            game::SetEventListener(&OnEvent, false);
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

        void Rebuild() {
            if (!g_haveNames) {
                Names n{EnumNames("EStatType"), EnumNames("EItemContainerType"), EnumNames("EWeaponType"), EnumNames("EWeaponDamageType")};
                if (n.stat.empty() || n.container.empty()) return;
                std::string s = "[item-sort] enums: stats " + std::to_string(n.stat.size()) + ", containers";
                for (size_t i = 0; i < n.container.size(); i++) s += " " + std::to_string(i) + "=" + n.container[i];
                s += ", damage types";
                for (size_t i = 0; i < n.damageType.size(); i++) s += " " + std::to_string(i) + "=" + n.damageType[i];
                s += ", weapon types";
                for (size_t i = 0; i < n.weaponType.size(); i++) s += " " + std::to_string(i) + "=" + n.weaponType[i];
                logger::log(s);
                AcquireSRWLockExclusive(&g_mu); g_names = std::move(n); g_haveNames = true; ReleaseSRWLockExclusive(&g_mu);
            }
            items = BuildModel();
            if (items.size() != loggedCount) { LogModel(items); loggedCount = items.size(); }
        }

        void RequestSort(bool storage) {
            AcquireSRWLockExclusive(&g_mu);
            g_req = {storage, profiles[cur]};
            g_status = "sorting...";
            ReleaseSRWLockExclusive(&g_mu);
            if (!g_on) { g_on = true; game::SetEventListener(&OnEvent, true); }
            g_pending = true;
        }

        void Weights(Profile& p) {
            bool dirty = false;
            for (int a = 1; a < 4; a++) {
                float v = AttackWeight(p, Attack(a));
                if (ImGui::SliderFloat((std::string(kAttackName[a]) + " weapons").c_str(), &v, 0, 2, "%.2f")) { p.attack[a] = v; dirty = true; }
            }
            std::vector<std::string> seen;  // stats present on the player's items, by name
            for (const Item& it : items)
                for (const Stat& s : it.stats) seen.push_back(StatName(g_names.stat, s.type));
            std::sort(seen.begin(), seen.end());
            seen.erase(std::unique(seen.begin(), seen.end()), seen.end());
            for (const std::string& s : seen) {
                float v = Weight(p, s);
                if (ImGui::SliderFloat(s.c_str(), &v, 0, 2, "%.2f")) { p.weight[s] = v; dirty = true; }
            }
            if (ImGui::Button("Reset profile")) { p.weight.clear(); p.attack.clear(); dirty = true; }
            if (dirty) ImGui::MarkIniSettingsDirty();
        }

        void Table() {
            std::vector<int> rows;
            for (int i : order) if (Passes(items[i], filter)) rows.push_back(i);
            ImGui::TextDisabled("%d of %d items", int(rows.size()), int(items.size()));
            const ImGuiTableFlags fl = ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_SizingStretchProp;
            if (!ImGui::BeginTable("items", 5, fl, ImVec2(0, ImGui::GetTextLineHeightWithSpacing() * 14))) return;
            ImGui::TableSetupScrollFreeze(0, 1);
            ImGui::TableSetupColumn("Score");
            ImGui::TableSetupColumn("Item", ImGuiTableColumnFlags_WidthStretch, 4);
            ImGui::TableSetupColumn("Lv");
            ImGui::TableSetupColumn("Where");
            ImGui::TableSetupColumn("Type");
            ImGui::TableHeadersRow();
            ImGuiListClipper clip;
            clip.Begin(int(rows.size()));
            while (clip.Step())
                for (int r = clip.DisplayStart; r < clip.DisplayEnd; r++) {
                    const Item& it = items[rows[r]];
                    ImGui::TableNextRow();
                    ImGui::TableNextColumn(); ImGui::Text("%.1f", score[rows[r]]);
                    ImGui::TableNextColumn();
                    ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(style::Pack(style::rarity::Of(it.grade))), "%s", it.name.c_str());
                    if (ImGui::IsItemHovered()) {
                        ImGui::BeginTooltip();
                        ImGui::Text("grade %d  slot %d  type %d  spec %d", it.grade, it.slot, it.containerType, it.specId);
                        for (const Stat& s : it.stats) {
                            const std::string n = StatName(g_names.stat, s.type);
                            ImGui::Text("%s: %.1f x %.2f", n.c_str(), s.value, Weight(profiles[cur], n));
                        }
                        if (it.stats.empty()) ImGui::TextDisabled("no stats read");
                        ImGui::EndTooltip();
                    }
                    ImGui::TableNextColumn(); ImGui::Text("%d", it.level);
                    ImGui::TableNextColumn(); ImGui::TextUnformatted(kWhereName[int(it.where)]);
                    ImGui::TableNextColumn();
                    ImGui::TextUnformatted(it.kind == Kind::Weapon ? kAttackName[int(it.attack)] : kKindName[int(it.kind)]);
                }
            ImGui::EndTable();
        }

        void Menu() override {
            const double now = ImGui::GetTime();
            if (now >= nextBuild) { Rebuild(); nextBuild = now + 0.5; }
            Profile& p = profiles[cur];
            score.clear();
            for (const Item& it : items) score.push_back(Score(it, p, g_names.stat));
            order = Order(items, score);

            if (ImGui::BeginCombo("Profile", p.name.c_str())) {
                for (int i = 0; i < int(profiles.size()); i++)
                    if (ImGui::Selectable(profiles[i].name.c_str(), i == cur)) { cur = i; ImGui::MarkIniSettingsDirty(); }
                ImGui::EndCombo();
            }
            if (ImGui::TreeNode("Weights")) { Weights(p); ImGui::TreePop(); }

            for (int w = 0; w < 4; w++) {
                if (w) ImGui::SameLine();
                ImGui::Checkbox(kWhereName[w], &filter.where[w]);
            }
            static const char* const kKinds[] = {"All", "Other", "Weapon", "Armor"};
            static const char* const kAttacks[] = {"All", "-", "Melee", "Ranged", "Magic"};
            ImGui::SetNextItemWidth(ImGui::GetFontSize() * 7);
            int k = filter.kind + 1;
            if (ImGui::Combo("Kind", &k, kKinds, IM_ARRAYSIZE(kKinds))) filter.kind = k - 1;
            ImGui::SameLine();
            ImGui::SetNextItemWidth(ImGui::GetFontSize() * 7);
            int a = filter.attack + 1;
            if (ImGui::Combo("Attack", &a, kAttacks, IM_ARRAYSIZE(kAttacks))) filter.attack = a - 1;
            if (ImGui::InputText("Search", search, sizeof(search))) filter.text = search;

            if (!g_haveNames) { ImGui::TextDisabled("waiting for game data (enter the game world)"); return; }
            if (ImGui::Button("Sort inventory")) RequestSort(false);
            ImGui::SameLine();
            if (ImGui::Button("Sort bank")) RequestSort(true);
            AcquireSRWLockShared(&g_mu);
            const std::string st = g_status;
            ReleaseSRWLockShared(&g_mu);
            if (!st.empty()) ImGui::TextWrapped("%s", st.c_str());
            Table();
        }
    } g_item_sort;
}

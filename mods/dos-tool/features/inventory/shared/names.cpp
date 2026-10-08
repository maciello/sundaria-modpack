#include "sdk.hpp"
#include "logger.hpp"
#include <Windows.h>
#include <atomic>
#include <unordered_map>

// Enum and attribute names, loaded once on the render thread. Facts: references/game-facts.md § items.
using namespace items::sdk;
using namespace items::io;

namespace {
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
}

namespace items::sdk {
    const std::vector<int32>& AttrOffsets() { return g_attrOffset; }
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
}

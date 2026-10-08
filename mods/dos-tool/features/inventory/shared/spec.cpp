#include "sdk.hpp"
#include "logger.hpp"
#include "game.hpp"
#include "cost.hpp"
#include <unordered_map>
#include "BP_SpecItemBase_classes.hpp"
#include "BP_SpecItemBase_parameters.hpp"

using namespace items::sdk;

namespace {
    // Spec id → spec from the item spec manager (BP_SpecManagerItem_C, found once by game::FindSingleton and cached).
    // The id map is rebuilt from its mLoadedSpecMap (O(specs)) only when an id misses or a cached spec died.
    struct SpecRef { UArchonSpec* sp; int32 idx; };
    std::unordered_map<int, SpecRef> g_specs;
    UArchonSpecManager* g_mgr = nullptr;
    int32 g_mgrIdx = -1;
    bool Live(const UObject* o, int32 idx) { return PtrOk(o) && UObject::GObjects->GetByIndex(idx) == o; }
    void BuildSpecs() {  // O(loaded specs); only on a miss
        static cost::Path path{"items spec map rebuild"};
        cost::Scope cs(path);
        if (!Live(g_mgr, g_mgrIdx)) {
            auto* m = static_cast<UObject*>(game::FindSingleton("BP_SpecManagerItem_C"));
            g_mgr = PtrOk(m) && m->IsA(UArchonSpecManager::StaticClass()) ? static_cast<UArchonSpecManager*>(m) : nullptr;
            g_mgrIdx = g_mgr ? g_mgr->Index : -1;
        }
        g_specs.clear();
        if (g_mgr)
            ForEach(g_mgr->mLoadedSpecMap, [&](int32 id, UArchonSpec* sp) {
                if (PtrOk(sp)) g_specs[id] = {sp, sp->Index};
            });
        logger::log("[items] spec map rebuilt: " + I(g_specs.size()) + " specs");
    }
}

namespace items::sdk {
    UArchonSpec* Spec(int id, bool& rebuilt) {  // O(1) hash hit
        static cost::Path path{"items spec lookup"};
        cost::Scope cs(path);
        auto it = g_specs.find(id);
        if (it != g_specs.end() && Live(it->second.sp, it->second.idx)) return it->second.sp;
        if (rebuilt) return nullptr;  // one rebuild per read
        rebuilt = true;
        BuildSpecs();
        it = g_specs.find(id);
        return it != g_specs.end() ? it->second.sp : nullptr;
    }
}

namespace items::io {
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
}

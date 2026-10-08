#include "sdk.hpp"
#include "ref.hpp"
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
    std::unordered_map<int, ref::Ref> g_specs;  // UArchonSpec
    ref::Ref g_mgr;  // UArchonSpecManager
    void BuildSpecs() {  // O(loaded specs); only on a miss
        static cost::Path path{"items spec map rebuild"};
        cost::Scope cs(path);
        auto* mgr = g_mgr.Get<UArchonSpecManager>();
        if (!mgr) {
            auto* m = static_cast<UObject*>(game::FindSingleton("BP_SpecManagerItem_C"));
            mgr = PtrOk(m) && m->IsA(UArchonSpecManager::StaticClass()) ? static_cast<UArchonSpecManager*>(m) : nullptr;
            g_mgr = ref::Ref(mgr);
        }
        g_specs.clear();
        if (mgr)
            ForEach(mgr->mLoadedSpecMap, [&](int32 id, UArchonSpec* sp) {
                if (PtrOk(sp)) g_specs[id] = ref::Ref(sp);
            });
        logger::log("[items] spec map rebuilt: " + I(g_specs.size()) + " specs");
    }
}

namespace items::sdk {
    UArchonSpec* Spec(int id, bool& rebuilt) {  // O(1) hash hit
        static cost::Path path{"items spec lookup"};
        cost::Scope cs(path);
        auto it = g_specs.find(id);
        if (it != g_specs.end())
            if (auto* sp = it->second.Get<UArchonSpec>()) return sp;
        if (rebuilt) return nullptr;  // one rebuild per read
        rebuilt = true;
        BuildSpecs();
        it = g_specs.find(id);
        return it != g_specs.end() ? it->second.Get<UArchonSpec>() : nullptr;
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
    int IconId(int specId) {
        static std::unordered_map<int, int> cache;
        if (auto it = cache.find(specId); it != cache.end()) return it->second;
        bool rebuilt = false;
        UArchonSpec* sp = Spec(specId, rebuilt);
        int id = -1;
        if (sp && sp->IsA(UBP_SpecItemBase_C::StaticClass()))
            if (UFunction* fn = sp->Class->GetFunction("BP_SpecItemBase_C", "I_GetIconID")) {
                Params::BP_SpecItemBase_C_I_GetIconID p{};
                sp->ProcessEvent(fn, &p);
                id = p.ID;
            }
        if (sp) cache[specId] = id;  // a missing spec may load later
        return id;
    }
}

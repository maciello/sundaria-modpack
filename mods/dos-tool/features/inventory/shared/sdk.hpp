#pragma once
// SDK helpers shared by the item read units (names, spec, read, probe). Game/render thread as each caller states.
#include "io.hpp"
#include <string>
#include "Engine_classes.hpp"
#include "ArchonSpecSystem_classes.hpp"
#include "BP_PlayerControllerOnline_classes.hpp"
#include "BP_ItemContainerComponent_classes.hpp"

namespace items::sdk {
    using namespace SDK;
    inline bool PtrOk(const void* p) {
        const uintptr_t v = reinterpret_cast<uintptr_t>(p);
        return v > 0x10000 && v < 0x7FFFFFFFFFFFull;
    }
    inline std::string I(long long v) { return std::to_string(v); }
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
    ABP_PlayerControllerOnline_C* LocalPC();          // read.cpp
    UBP_ItemContainerComponent_C* Container(UObject* o);  // read.cpp: o if it is an item container
    const std::vector<int32>& AttrOffsets();          // names.cpp: parallel to GetNames().stat; valid once Ready()
    UArchonSpec* Spec(int id, bool& rebuilt);         // spec.cpp: one map rebuild per read (pass rebuilt=false per read)
}

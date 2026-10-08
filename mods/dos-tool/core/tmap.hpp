#pragma once
// SDK TMap walk for .cpp files that already include the SDK (game or render thread, plain memory reads).
#include <cstdint>
#include "Basic.hpp"

namespace tmap {
    // Dumper-7's TMap iterator does not compile (SetElement::Value is private): walk the sparse array directly.
    template <class K, class V, class F> void ForEach(const UC::TMap<K, V>& m, F&& f) {
        using Elem = UC::ContainerImpl::SetElement<UC::TPair<K, V>>;  // {TPair, HashNextId, HashIndex}
        const uint8_t* data = *reinterpret_cast<const uint8_t* const*>(&m);
        const uintptr_t v = reinterpret_cast<uintptr_t>(data);
        if (v <= 0x10000 || v >= 0x7FFFFFFFFFFFull) return;
        for (int i = 0; i < m.NumAllocated(); i++)
            if (m.IsValidIndex(i)) {
                auto& kv = *reinterpret_cast<const UC::TPair<K, V>*>(data + i * sizeof(Elem));
                f(kv.Key(), kv.Value());
            }
    }
}

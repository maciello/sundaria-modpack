#pragma once
// SDK helpers shared by the item read units (names, spec, read, probe). Game/render thread as each caller states.
#include "io.hpp"
#include "tmap.hpp"
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
    using tmap::ForEach;
    ABP_PlayerControllerOnline_C* LocalPC();          // read.cpp
    UBP_ItemContainerComponent_C* Container(UObject* o);  // read.cpp: o if it is an item container
    const std::vector<int32>& AttrOffsets();          // names.cpp: parallel to GetNames().stat; valid once Ready()
    std::vector<std::string> EnumNames(UEnum* e);  // names.cpp: value → display name (BP enums are dumped as NewEnumeratorN; UUserDefinedEnum keeps the names)
    UArchonSpec* Spec(int id, bool& rebuilt);         // spec.cpp: one map rebuild per read (pass rebuilt=false per read)
}

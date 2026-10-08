#pragma once
// Reflected property access (FProperty walk), the one copy shared by the probes and the live bridge. Plain memory
// reads; callers decide the thread (world objects: game thread).
#include <string>
#include <vector>

namespace SDK { class UStruct; class FProperty; }

namespace reflect {
    // Properties of a struct/class, own first, then each super's (withSupers).
    std::vector<const SDK::FProperty*> Props(const SDK::UStruct* s, bool withSupers = true);
    std::string Type(const SDK::FProperty* p);  // FField class name: "FloatProperty", "ObjectProperty" …⊇
    std::string Name(const SDK::FProperty* p);
}

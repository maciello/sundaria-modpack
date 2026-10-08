#include "ref.hpp"

#include <Windows.h>
#include "game.hpp"
#include "logger.hpp"
#include "umg.hpp"
#include "CoreUObject_classes.hpp"

using namespace SDK;

namespace ref::detail {
    const void* At(int32_t idx) { return UObject::GObjects ? UObject::GObjects->GetByIndex(idx) : nullptr; }
    int32_t IndexOf(const void* o) { return static_cast<const UObject*>(o)->Index; }
    uint64_t NameOf(const void* o) { return *reinterpret_cast<const uint64_t*>(&static_cast<const UObject*>(o)->Name); }
    uint64_t NowMs() { return GetTickCount64(); }
    bool MayResolve() { return game::OnGameThread(); }
    bool Ok(const void* p) { return umg::PtrOk(p); }
    const void* Function(UClass* (*cls)(), const char* clsName, const char* fnName) {
        UClass* c = cls();  // Blueprint classes: null until a world loads them (#54)
        return umg::PtrOk(c) ? c->GetFunction(clsName, fnName) : nullptr;
    }
    void Reloaded(const void* o, int32_t oldIdx) {
        logger::log("[ref] reloaded " + static_cast<const UObject*>(o)->GetName() + " (GObjects " + std::to_string(oldIdx) + " -> "
                    + std::to_string(IndexOf(o)) + ")");
    }
}

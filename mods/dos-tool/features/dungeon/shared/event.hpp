#pragma once
// A Blueprint function matched by FName (its name + its class's name), never by StaticClass(): a Blueprint StaticClass()
// searches GObjects on first use, and on every use while the class is not loaded. Game thread: making an FName is a
// ProcessEvent call (Warm() before the hot path, else the first Is() makes them).
#include "umg.hpp"
#include "CoreUObject_classes.hpp"

namespace dungeon_map {
    struct Event {
        const SDK::FName& (*cls)();  // the Blueprint class's StaticName
        const wchar_t* name;
        SDK::FName fn{};
        void Warm() { SDK::GetStaticName(name, fn), cls(); }
        bool Is(const void* f) {
            auto* o = static_cast<const SDK::UObject*>(f);
            return umg::PtrOk(o) && o->Name == SDK::GetStaticName(name, fn) && umg::PtrOk(o->Outer) && o->Outer->Name == cls();
        }
    };
}

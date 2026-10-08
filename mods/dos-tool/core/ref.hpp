#pragma once
// Engine pointers kept beyond the current call (UFunction, UClass, singletons, actors, widgets …⊇) live only in these.
// Map travel collects Blueprint classes, their UFunctions and world objects; the allocator and the GObjects slot are
// then reused by other objects, so a raw cached pointer silently names something else (#63: Received_Notify cached in
// town was a parameterless function in the dungeon). PtrOk only rejects null and garbage, never a freed object.
// Get() is O(1): GObjects[idx] == ptr and the FName is unchanged, else null. A torn read (render thread vs game
// thread) also fails that check, so it yields null, never a wrong object. Cached/Fn resolve on the game thread only
// (listeners also run on worker threads): elsewhere a stale entry reads as null.
#include <cstddef>
#include <cstdint>

namespace SDK { class UObject; class UClass; class UFunction; }

namespace ref {
    namespace detail {  // ref.cpp (SDK), or a fake in core/test/ref_test.cpp
        const void* At(int32_t idx);  // GObjects[idx], null if out of range or free
        int32_t IndexOf(const void* o);
        uint64_t NameOf(const void* o);  // FName {ComparisonIndex, Number}
        uint64_t NowMs();
        bool MayResolve();  // game thread
        bool Ok(const void* p);  // null/garbage filter (PtrOk)
        const void* Function(SDK::UClass* (*cls)(), const char* clsName, const char* fnName);
        void Reloaded(const void* o, int32_t oldIdx);  // log line: a cached object was collected and found again
    }

    struct Ref {
        const void* ptr = nullptr;
        int32_t idx = -1;
        uint64_t name = 0;

        Ref() = default;
        explicit Ref(const void* o) {
            if (!detail::Ok(o)) return;
            ptr = o; idx = detail::IndexOf(o); name = detail::NameOf(o);
        }
        // The object, if it is still the one stored; null once collected or its slot reused.
        template<class T = SDK::UObject> T* Get() const {
            if (!ptr || detail::At(idx) != ptr || detail::NameOf(ptr) != name) return nullptr;
            return static_cast<T*>(const_cast<void*>(ptr));
        }
        bool Is(const void* o) const { return o && o == Get<void>(); }  // o is the stored, still-live object
        bool operator==(const Ref& o) const { return ptr == o.ptr && idx == o.idx && name == o.name; }
    };
    // Key for maps of engine objects: Ref(live object) never matches an entry left by a collected one at its address.
    struct Hash {
        size_t operator()(const Ref& r) const { return reinterpret_cast<size_t>(r.ptr) ^ (size_t(uint32_t(r.idx)) << 1); }
    };

    // Shared by Cached and Fn: the stored object, else resolve() again (a null result at most once per second:
    // a Blueprint StaticClass() miss searches GObjects).
    template<class T, class F> T* Fetch(Ref& r, uint64_t& retryAt, F&& resolve) {
        if (T* o = r.Get<T>()) return o;
        if (!detail::MayResolve()) return nullptr;
        const uint64_t now = detail::NowMs();
        if (now < retryAt) return nullptr;
        retryAt = now + 1000;
        const Ref n(resolve());
        if (!n.ptr) return nullptr;  // keep the old entry: its slot shows in the reload log line
        if (r.ptr) detail::Reloaded(n.ptr, r.idx);
        r = n;
        return r.Get<T>();
    }

    template<class T> struct Cached {
        T* (*resolve)();
        Ref r{};
        uint64_t retryAt = 0;
        T* Get() { return Fetch<T>(r, retryAt, resolve); }
    };

    // A UFunction by class + name. The class may not be loaded yet (Blueprint classes are null at the main menu).
    struct Fn {
        SDK::UClass* (*cls)();
        const char* clsName;
        const char* fnName;
        Ref r{};
        uint64_t retryAt = 0;
        SDK::UFunction* Get() { return Fetch<SDK::UFunction>(r, retryAt, [this] { return detail::Function(cls, clsName, fnName); }); }
        bool Is(const void* fn) { return fn && fn == Get(); }
    };
}

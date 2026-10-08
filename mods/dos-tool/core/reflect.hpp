#pragma once
// Reflected property access (FProperty walk), the one copy shared by the probes and the live bridge. Plain memory
// reads; callers decide the thread (world objects: game thread).
#include <string>
#include <vector>

namespace SDK { class UObject; class UStruct; class FProperty; }

namespace reflect {
    // Properties of a struct/class, own first, then each super's (withSupers).
    std::vector<const SDK::FProperty*> Props(const SDK::UStruct* s, bool withSupers = true);
    std::string Type(const SDK::FProperty* p);  // FField class name: "FloatProperty", "ObjectProperty" …⊇
    std::string Name(const SDK::FProperty* p);
    // By name, case-insensitive; a user-defined struct's "Name_<n>_<GUID>" also answers to "Name". null = none.
    const SDK::FProperty* Find(const SDK::UStruct* s, const std::string& name);

    // Value at addr as JSON. depth = levels of structs/arrays/objects expanded (0: struct name, array size).
    std::string Json(const SDK::FProperty* p, const void* addr, int depth);
    // Every property of an object: {"$class", "$name", <prop>: Json(depth)}.
    std::string ObjectJson(const SDK::UObject* o, int depth);
    // Object a pointer-like property (object, class, interface, weak, lazy, soft) points at; null otherwise.
    SDK::UObject* Deref(const SDK::FProperty* p, const void* addr);

    // Path step: "Name" or "Name[i]" (TArray element or fixed-size array slot).
    struct Step { std::string name; int index = -1; };
    // Where a path ends: prop == nullptr means the object itself. count > 1: a fixed-size array without an index.
    struct Value { const SDK::FProperty* prop = nullptr; const void* addr = nullptr; const SDK::UObject* obj = nullptr; int count = 1; };
    // Follows steps from root through object pointers, structs and arrays. false: err says where and what exists.
    bool Walk(const SDK::UObject* root, const std::vector<Step>& steps, Value& out, std::string& err);
    std::string JsonOf(const Value& v, int depth);  // objects expand via ObjectJson

    // Writes one scalar into a parameter: bool, ints, float, double, byte/enum (number or enumerator name), string.
    // strings: storage for FString arguments, kept alive by the caller until the call returns.
    struct Scalar { bool isStr = false; double num = 0; std::string str; };
    bool Set(const SDK::FProperty* p, void* addr, const Scalar& v, std::vector<std::wstring>& strings, std::string& err);

    // JSON string literal, quotes included.
    inline std::string Esc(const std::string& s) {
        static const char hex[] = "0123456789abcdef";
        std::string o = "\"";
        for (unsigned char c : s) {
            if (c == '"' || c == '\\') { o += '\\'; o += char(c); }
            else if (c == '\n') o += "\\n";
            else if (c == '\r') o += "\\r";
            else if (c == '\t') o += "\\t";
            else if (c < 0x20) { o += "\\u00"; o += hex[c >> 4]; o += hex[c & 15]; }
            else o += char(c);
        }
        return o + "\"";
    }
}

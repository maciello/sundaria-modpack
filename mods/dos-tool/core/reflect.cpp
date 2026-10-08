#include "reflect.hpp"
#include "umg.hpp"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <new>
#include "CoreUObject_classes.hpp"

using namespace SDK;
using umg::PtrOk;

namespace {
    using Bytes = const uint8*;
    template <class T> T At(const void* a) { T v; std::memcpy(&v, a, sizeof v); return v; }

    std::string Num(double v, const char* fmt) {
        if (!std::isfinite(v)) return reflect::Esc(std::isnan(v) ? "nan" : v > 0 ? "inf" : "-inf");
        char b[40];
        std::snprintf(b, sizeof b, fmt, v);
        return b;
    }
    std::string Int(long long v) { return std::to_string(v); }

    std::string EnumName(const UEnum* e, long long v) {
        if (PtrOk(e))
            for (int i = 0; i < e->Names.Num(); i++)
                if (e->Names[i].Value() == v) {
                    std::string n = e->Names[i].Key().ToString();
                    if (auto p = n.rfind("::"); p != std::string::npos) n = n.substr(p + 2);
                    return reflect::Esc(n);
                }
        return Int(v);
    }
    long long Signed(const void* a, int size) {
        switch (size) { case 1: return At<int8>(a); case 2: return At<int16>(a); case 4: return At<int32>(a); default: return At<int64>(a); }
    }

    std::string Ref(const UObject* o) {
        return PtrOk(o) && PtrOk(o->Class) ? reflect::Esc(o->Class->GetName() + " " + o->GetName()) : "null";
    }

    bool IsObject(const std::string& t) {
        return t == "ObjectProperty" || t == "ClassProperty" || t == "InterfaceProperty" || t == "WeakObjectProperty"
            || t == "LazyObjectProperty" || t == "SoftObjectProperty" || t == "SoftClassProperty";
    }
    bool Matches(const std::string& name, const std::string& want) {
        if (name.size() == want.size()) return !_stricmp(name.c_str(), want.c_str());
        // user-defined struct fields: Name_<n>_<32 hex GUID>
        return name.size() > want.size() + 34 && !_strnicmp(name.c_str(), want.c_str(), want.size()) && name[want.size()] == '_';
    }
    std::string StructName(const UStruct* s) { return PtrOk(s) ? s->GetName() : "?"; }
}

namespace reflect {
    std::vector<const FProperty*> Props(const UStruct* s, bool withSupers) {
        std::vector<const FProperty*> out;
        for (const UStruct* c = s; PtrOk(c); c = withSupers ? c->SuperStruct : nullptr)
            for (const FField* f = c->ChildProperties; PtrOk(f); f = f->Next) out.push_back(static_cast<const FProperty*>(f));
        return out;
    }
    std::string Type(const FProperty* p) { return PtrOk(p) && PtrOk(p->ClassPrivate) ? p->ClassPrivate->Name.ToString() : "?"; }
    std::string Name(const FProperty* p) { return PtrOk(p) ? p->Name.ToString() : "?"; }

    const FProperty* Find(const UStruct* s, const std::string& name) {
        const FProperty* loose = nullptr;
        for (const FProperty* p : Props(s)) {
            const std::string n = Name(p);
            if (n.size() == name.size() && Matches(n, name)) return p;
            if (!loose && Matches(n, name)) loose = p;
        }
        return loose;
    }

    UObject* Deref(const FProperty* p, const void* a) {
        const std::string t = Type(p);
        if (t == "ObjectProperty" || t == "ClassProperty" || t == "InterfaceProperty") {
            UObject* o = At<UObject*>(a);
            return PtrOk(o) ? o : nullptr;
        }
        // weak/lazy/soft pointers start with an FWeakObjectPtr
        if (IsObject(t)) return static_cast<const FWeakObjectPtr*>(a)->Get();
        return nullptr;
    }

    std::string Json(const FProperty* p, const void* a, int depth) {
        if (!PtrOk(p) || !PtrOk(a)) return "null";
        const std::string t = Type(p);
        if (t == "BoolProperty") {
            auto* b = static_cast<const FBoolProperty*>(p);
            return (static_cast<Bytes>(a)[b->ByteOffset] & b->FieldMask) ? "true" : "false";
        }
        if (t == "FloatProperty") return Num(At<float>(a), "%.9g");
        if (t == "DoubleProperty") return Num(At<double>(a), "%.17g");
        if (t == "IntProperty" || t == "Int8Property" || t == "Int16Property" || t == "Int64Property") return Int(Signed(a, p->ElementSize));
        if (t == "UInt16Property") return Int(At<uint16>(a));
        if (t == "UInt32Property") return Int(At<uint32>(a));
        if (t == "UInt64Property") return std::to_string(At<uint64>(a));
        if (t == "ByteProperty") {
            const UEnum* e = static_cast<const FByteProperty*>(p)->Enum;
            return PtrOk(e) ? EnumName(e, At<uint8>(a)) : Int(At<uint8>(a));
        }
        if (t == "EnumProperty") {
            auto* ep = static_cast<const FEnumProperty*>(p);
            const int size = PtrOk(ep->UnderlayingProperty) ? ep->UnderlayingProperty->ElementSize : 1;
            return EnumName(ep->Enum, size == 1 ? At<uint8>(a) : Signed(a, size));
        }
        if (t == "NameProperty") return Esc(static_cast<const FName*>(a)->ToString());
        if (t == "StrProperty") {
            auto* s = static_cast<const FString*>(a);
            return s->Num() > 0 && PtrOk(s->CStr()) ? Esc(s->ToString()) : "\"\"";
        }
        if (t == "TextProperty") {
            auto* x = static_cast<const FText*>(a);
            return PtrOk(x->TextData) ? Esc(x->ToString()) : "null";
        }
        if (IsObject(t)) {
            UObject* o = Deref(p, a);
            if (!o && (t == "SoftObjectProperty" || t == "SoftClassProperty"))  // not loaded: its path (FSoftObjectPath after the weak ptr + tag)
                return Esc("unloaded " + static_cast<const FName*>(static_cast<const void*>(static_cast<Bytes>(a) + 0x10))->ToString());
            return Ref(o);
        }
        if (t == "StructProperty") {
            const UStruct* s = static_cast<const FStructProperty*>(p)->Struct;
            if (depth <= 0) return Esc(StructName(s));
            std::string o = "{";
            for (const FProperty* f : Props(s)) {
                if (o.size() > 1) o += ",";
                o += Esc(Name(f)) + ":" + Json(f, static_cast<Bytes>(a) + f->Offset, depth - 1);
            }
            return o + "}";
        }
        if (t == "ArrayProperty") {
            const FProperty* inner = static_cast<const FArrayProperty*>(p)->InnerProperty;
            const Bytes data = At<Bytes>(a);
            const int n = At<int32>(static_cast<Bytes>(a) + 8);
            if (depth <= 0 || !PtrOk(inner) || !PtrOk(data)) return "{\"num\":" + Int(n) + "}";
            std::string o = "[";
            const int shown = n < 64 ? n : 64;
            for (int i = 0; i < shown; i++) o += (i ? "," : "") + Json(inner, data + i * inner->ElementSize, depth - 1);
            if (n > shown) o += "," + Esc("+" + Int(n - shown) + " more");
            return o + "]";
        }
        if (t == "MapProperty") return "{\"num\":" + Int(static_cast<const UC::TMap<int32, int32>*>(a)->Num()) + "}";
        if (t == "SetProperty") return "{\"num\":" + Int(static_cast<const UC::TSet<int32>*>(a)->Num()) + "}";
        return Esc("<" + t + ">");
    }

    std::string ObjectJson(const UObject* o, int depth) {
        if (!PtrOk(o) || !PtrOk(o->Class)) return "null";
        std::string s = "{\"$class\":" + Esc(o->Class->GetName()) + ",\"$name\":" + Esc(o->GetName());
        for (const FProperty* p : Props(o->Class)) {
            const Bytes a = reinterpret_cast<Bytes>(o) + p->Offset;
            s += "," + Esc(Name(p)) + ":";
            if (p->ArrayDim <= 1) { s += Json(p, a, depth); continue; }
            s += "[";
            for (int i = 0; i < p->ArrayDim; i++) s += (i ? "," : "") + Json(p, a + i * p->ElementSize, depth);
            s += "]";
        }
        return s + "}";
    }

    bool Walk(const UObject* root, const std::vector<Step>& steps, Value& out, std::string& err) {
        out = {nullptr, root, root, 1};
        if (!PtrOk(root) || !PtrOk(root->Class)) { err = "root is null"; return false; }
        const UStruct* st = root->Class;
        Bytes base = reinterpret_cast<Bytes>(root);
        std::string at = "root";
        for (const Step& s : steps) {
            if (out.prop) {  // step into the current value
                if (UObject* o = Deref(out.prop, out.addr)) { st = o->Class; base = reinterpret_cast<Bytes>(o); out.obj = o; }
                else if (Type(out.prop) == "StructProperty") { st = static_cast<const FStructProperty*>(out.prop)->Struct; base = static_cast<Bytes>(out.addr); }
                else if (IsObject(Type(out.prop))) { err = at + " is null"; return false; }
                else { err = at + " is " + Type(out.prop) + ", it has no fields"; return false; }
            }
            const FProperty* p = Find(st, s.name);
            if (!p) {
                err = "no " + s.name + " on " + StructName(st) + " (" + at + "); it has:";
                for (const FProperty* f : Props(st)) err += " " + Name(f);
                return false;
            }
            at += "." + Name(p);
            out.prop = p;
            out.addr = base + p->Offset;
            out.count = p->ArrayDim;
            if (s.index < 0) continue;
            at += "[" + std::to_string(s.index) + "]";
            if (p->ArrayDim > 1) {
                if (s.index >= p->ArrayDim) { err = at + ": out of range (size " + std::to_string(p->ArrayDim) + ")"; return false; }
                out.addr = static_cast<Bytes>(out.addr) + s.index * p->ElementSize;
            } else if (Type(p) == "ArrayProperty") {
                const FProperty* inner = static_cast<const FArrayProperty*>(p)->InnerProperty;
                const int n = At<int32>(static_cast<Bytes>(out.addr) + 8);
                if (s.index >= n || !PtrOk(inner)) { err = at + ": out of range (num " + std::to_string(n) + ")"; return false; }
                out.prop = inner;
                out.addr = At<Bytes>(out.addr) + s.index * inner->ElementSize;
            } else { err = at + " is " + Type(p) + ", not an array"; return false; }
            out.count = 1;
        }
        return true;
    }

    std::string JsonOf(const Value& v, int depth) {
        if (!v.prop) return ObjectJson(v.obj, depth);
        if (v.count > 1) {
            std::string s = "[";
            for (int i = 0; i < v.count; i++) s += (i ? "," : "") + Json(v.prop, static_cast<Bytes>(v.addr) + i * v.prop->ElementSize, depth);
            return s + "]";
        }
        if (UObject* o = Deref(v.prop, v.addr)) return ObjectJson(o, depth > 0 ? depth - 1 : 0);
        return Json(v.prop, v.addr, depth);
    }

    bool Set(const FProperty* p, void* a, const Scalar& v, std::vector<std::wstring>& strings, std::string& err) {
        const std::string t = Type(p);
        auto* b = static_cast<uint8*>(a);
        const auto put = [&](auto x) { std::memcpy(a, &x, sizeof x); return true; };
        if (t == "StrProperty") {
            if (!v.isStr) { err = Name(p) + ": wants a string"; return false; }
            strings.emplace_back(v.str.begin(), v.str.end());  // ponytail: ASCII only, MultiByteToWideChar when a UTF-8 arg is needed
            new (a) FString(strings.back().c_str());
            return true;
        }
        long long n = (long long)v.num;
        if (v.isStr) {
            const UEnum* e = t == "ByteProperty" ? static_cast<const FByteProperty*>(p)->Enum
                           : t == "EnumProperty" ? static_cast<const FEnumProperty*>(p)->Enum : nullptr;
            bool found = false;
            for (int i = 0; PtrOk(e) && i < e->Names.Num() && !found; i++) {
                std::string k = e->Names[i].Key().ToString();
                if (auto q = k.rfind("::"); q != std::string::npos) k = k.substr(q + 2);
                if (!_stricmp(k.c_str(), v.str.c_str())) { n = e->Names[i].Value(); found = true; }
            }
            if (!found) { err = Name(p) + " (" + t + "): wants a number" + (PtrOk(e) ? " or an enumerator of " + e->GetName() : ""); return false; }
        }
        if (t == "BoolProperty") {
            auto* bp = static_cast<const FBoolProperty*>(p);
            b[bp->ByteOffset] = n ? (b[bp->ByteOffset] | bp->FieldMask) : (b[bp->ByteOffset] & ~bp->FieldMask);
            return true;
        }
        if (t == "FloatProperty") return put(float(v.num));
        if (t == "DoubleProperty") return put(double(v.num));
        if (t == "EnumProperty") {
            auto* ep = static_cast<const FEnumProperty*>(p);
            const int size = PtrOk(ep->UnderlayingProperty) ? ep->UnderlayingProperty->ElementSize : 1;
            std::memcpy(a, &n, size);  // little endian: the low bytes
            return true;
        }
        if (t == "ByteProperty" || t == "Int8Property" || t == "Int16Property" || t == "IntProperty" || t == "Int64Property"
            || t == "UInt16Property" || t == "UInt32Property" || t == "UInt64Property") {
            std::memcpy(a, &n, p->ElementSize);
            return true;
        }
        err = Name(p) + ": " + t + " arguments are not supported (bool, numbers, enums, strings are)";
        return false;
    }
}

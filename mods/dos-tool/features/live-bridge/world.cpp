#include "live-bridge.hpp"
#include "ref.hpp"
#include "reflect.hpp"
#include "umg.hpp"

#include <Windows.h>
#include <algorithm>
#include <cmath>
#include <unordered_map>
#include "Engine_classes.hpp"

// Live bridge, game thread: get / find / call, and the ProcessEvent trace. Called from the bridge's listener only
// (game::OnGameThread, world tick for commands), so actors are read while the game cannot free them.
using namespace SDK;
using umg::PtrOk;

namespace {
    std::vector<ref::Ref> g_found;  // `find` results: @i roots

    std::string F(double v) { char b[32]; std::snprintf(b, sizeof b, "%.2f", v); return b; }

    UObject* Root(const std::string& r, std::string& err) {
        if (r == "pc") return umg::LocalPC();
        if (r == "world") return UWorld::GetWorld();
        if (r.size() > 1 && r[0] == '@') {
            const int i = std::atoi(r.c_str() + 1);
            if (UObject* o = i >= 0 && i < int(g_found.size()) ? g_found[i].Get<UObject>() : nullptr) return o;
            err = r + " is gone or out of range (" + std::to_string(g_found.size()) + " found): run find again";
            return nullptr;
        }
        if (!r.compare(0, 4, "obj:"))  // dev only: one GObjects lookup per command, by short object name
            if (UObject* o = UObject::FindObjectFast<UObject>(r.substr(4))) return o;
        err = "no root " + r + "; roots: pc pawn ps cam hud world gi gs @<i from find> obj:<ObjectName>";
        return nullptr;
    }

    // Resolves a path; false: err.
    bool Resolve(const std::string& text, reflect::Value& v, std::string& err) {
        live_bridge::Path p = live_bridge::ParsePath(text);
        if (!p.err.empty()) { err = "path: " + p.err; return false; }
        live_bridge::ExpandAlias(p);
        UObject* root = Root(p.root, err);
        if (!root) { if (err.empty()) err = p.root + " is null (not in a level?)"; return false; }
        return reflect::Walk(root, p.steps, v, err);
    }

    std::string Get(const std::vector<std::string>& w) {
        if (w.size() < 2) return live_bridge::Err("get <path> [depth]");
        reflect::Value v;
        std::string err;
        if (!Resolve(w[1], v, err)) return live_bridge::Err(err);
        const int depth = w.size() > 2 ? std::clamp(std::atoi(w[2].c_str()), 0, 4) : 1;
        return live_bridge::Ok(reflect::JsonOf(v, depth));
    }

    bool ClassMatches(const UObject* a, const std::string& q) {
        if (_stricmp(a->Class->GetName().c_str(), q.c_str()) == 0) return true;
        for (const UStruct* c = a->Class->SuperStruct; PtrOk(c); c = c->SuperStruct)
            if (_stricmp(c->GetName().c_str(), q.c_str()) == 0) return true;
        std::string n = a->Class->GetName(), l = q;  // substring of the actor's own class
        for (auto* s : {&n, &l}) std::transform(s->begin(), s->end(), s->begin(), [](unsigned char c) { return char(std::tolower(c)); });
        return n.find(l) != std::string::npos;
    }

    // Loaded levels' actors whose class (or a super) is q, or whose class name contains q; nearest to the pawn first.
    std::string FindActors(const std::string& q, double nearM) {
        UWorld* w = UWorld::GetWorld();
        if (!PtrOk(w)) return live_bridge::Err("no world");
        APlayerController* pc = umg::LocalPC();
        const APawn* me = PtrOk(pc) && PtrOk(pc->Pawn) ? pc->Pawn : nullptr;
        const bool haveMe = me && PtrOk(me->RootComponent);
        const FVector p0 = haveMe ? me->RootComponent->RelativeLocation : FVector{};
        struct Hit { double m; AActor* a; };
        std::vector<Hit> hits;
        for (int li = 0; li < w->Levels.Num(); li++) {
            ULevel* lvl = w->Levels[li];
            if (!PtrOk(lvl)) continue;
            for (int ai = 0; ai < lvl->Actors.Num(); ai++) {
                AActor* a = lvl->Actors[ai];
                if (!PtrOk(a) || !PtrOk(a->Class) || !ClassMatches(a, q)) continue;
                double m = -1;
                if (haveMe && PtrOk(a->RootComponent)) {
                    const FVector p = a->RootComponent->RelativeLocation;
                    m = std::sqrt(double(p.X - p0.X) * (p.X - p0.X) + double(p.Y - p0.Y) * (p.Y - p0.Y) + double(p.Z - p0.Z) * (p.Z - p0.Z)) / 100.0;
                }
                if (nearM > 0 && m > nearM) continue;
                hits.push_back({m, a});
            }
        }
        std::sort(hits.begin(), hits.end(), [](const Hit& a, const Hit& b) { return a.m < b.m; });
        const size_t shown = std::min<size_t>(hits.size(), 40);
        g_found.clear();
        std::string s = "{\"total\":" + std::to_string(hits.size()) + ",\"actors\":[";
        for (size_t i = 0; i < shown; i++) {
            AActor* a = hits[i].a;
            g_found.emplace_back(a);
            s += (i ? "," : "") + std::string("{\"@\":") + std::to_string(i) + ",\"class\":" + reflect::Esc(a->Class->GetName())
               + ",\"name\":" + reflect::Esc(a->GetName()) + ",\"m\":" + (hits[i].m < 0 ? "null" : F(hits[i].m));
            if (PtrOk(a->RootComponent)) {
                const FVector p = a->RootComponent->RelativeLocation;
                s += ",\"at\":[" + F(p.X) + "," + F(p.Y) + "," + F(p.Z) + "]";
            }
            s += "}";
        }
        return live_bridge::Ok(s + "]}");
    }

    std::string Find(const std::vector<std::string>& w) {
        if (w.size() < 2) return live_bridge::Err("find <class> [near <m>]  (near 0 = any distance; default 50)");
        double nearM = 50;
        if (w.size() > 3 && w[2] == "near") nearM = std::atof(w[3].c_str());
        return FindActors(w[1], nearM);
    }

    UFunction* FindFn(const UObject* o, const std::string& name, std::string& have) {
        for (const UStruct* c = o->Class; PtrOk(c); c = c->SuperStruct)
            for (UField* f = c->Children; PtrOk(f); f = f->Next) {
                if (!f->IsA(EClassCastFlags::Function)) continue;
                if (!_stricmp(f->GetName().c_str(), name.c_str())) return static_cast<UFunction*>(f);
                if (have.size() < 4000) have += " " + f->GetName();
            }
        return nullptr;
    }

    constexpr uint64_t kParm = 0x80, kOutParm = 0x100, kReturnParm = 0x400, kReferenceParm = 0x8000000;

    std::string Call(const std::string& line, const std::vector<std::string>& w, const std::vector<size_t>& at) {
        if (w.size() < 3) return live_bridge::Err("call <path> <Function> [json args]");
        APlayerController* pc = umg::LocalPC();
        if (!PtrOk(pc) || pc->Role != ENetRole::ROLE_Authority) return live_bridge::Err("not authority (co-op client): call runs on the host only");
        reflect::Value v;
        std::string err;
        if (!Resolve(w[1], v, err)) return live_bridge::Err(err);
        UObject* obj = v.prop ? reflect::Deref(v.prop, v.addr) : const_cast<UObject*>(v.obj);
        if (!PtrOk(obj)) return live_bridge::Err(w[1] + " is not an object");
        std::string have;
        UFunction* fn = FindFn(obj, w[2], have);
        if (!fn) return live_bridge::Err("no function " + w[2] + " on " + obj->Class->GetName() + "; it has:" + have);
        std::vector<reflect::Scalar> args;
        if (w.size() > 3 && !live_bridge::ParseArgs(line.substr(at[3]), args, err)) return live_bridge::Err("args: " + err);

        std::string sig;
        const std::vector<const FProperty*> props = reflect::Props(fn, false);
        for (const FProperty* p : props)
            if ((p->PropertyFlags & kParm) && !(p->PropertyFlags & kReturnParm))
                sig += " " + reflect::Name(p) + ":" + reflect::Type(p) + ((p->PropertyFlags & kOutParm) && !(p->PropertyFlags & kReferenceParm) ? "(out)" : "");
        std::vector<uint64_t> buf(size_t(fn->Size) / 8 + 2, 0);  // zeroed, 8-aligned parameter block
        auto* parms = reinterpret_cast<uint8*>(buf.data());
        std::vector<std::wstring> strings;
        size_t used = 0;
        for (const FProperty* p : props) {
            const uint64_t f = p->PropertyFlags;
            if (!(f & kParm) || (f & kReturnParm) || ((f & kOutParm) && !(f & kReferenceParm))) continue;
            if (used >= args.size()) return live_bridge::Err(fn->GetName() + " wants:" + sig);
            if (!reflect::Set(p, parms + p->Offset, args[used++], strings, err)) return live_bridge::Err(err + "; " + fn->GetName() + " wants:" + sig);
        }
        if (used < args.size()) return live_bridge::Err("too many args; " + fn->GetName() + " wants:" + (sig.empty() ? " nothing" : sig));

        obj->ProcessEvent(fn, parms);

        std::string ret = "null", out;
        for (const FProperty* p : props) {
            const uint64_t f = p->PropertyFlags;
            if (f & kReturnParm) ret = reflect::Json(p, parms + p->Offset, 2);
            else if (f & kOutParm) out += (out.empty() ? "" : ",") + reflect::Esc(reflect::Name(p)) + ":" + reflect::Json(p, parms + p->Offset, 2);
        }
        // ponytail: out/return FString/TArray buffers the engine allocated are leaked (dev tool, a few bytes per call)
        return live_bridge::Ok("{\"on\":" + reflect::Esc(obj->Class->GetName() + " " + obj->GetName()) + ",\"return\":" + ret + ",\"out\":{" + out + "}}");
    }

    // trace: game thread writes, the bridge thread reads after the window; one lock, uncontended while tracing
    SRWLOCK g_traceMu = SRWLOCK_INIT;
    struct Hit { std::string fn, on; int count = 0; double first = 0, last = 0; };
    std::unordered_map<uintptr_t, bool> g_match;  // UFunction address -> name matches (ponytail: address reuse after map travel mid-trace mislabels)
    std::unordered_map<uintptr_t, Hit> g_hits;    // UFunction x object class
    ULONGLONG g_t0 = 0;
    std::string g_pattern;
}

namespace live_bridge {
    std::string RunOnGameThread(const std::string& line) {
        std::vector<size_t> at;
        const std::vector<std::string> w = Words(line, &at);
        if (w[0] == "get") return Get(w);
        if (w[0] == "find") return Find(w);
        if (w[0] == "call") return Call(line, w, at);
        return Err("unknown game-thread command " + w[0]);
    }

    bool TraceBegin(const std::string& pattern, std::string& err) {
        AcquireSRWLockExclusive(&g_traceMu);
        g_pattern = pattern;
        g_match.clear();
        g_hits.clear();
        g_t0 = GetTickCount64();
        ReleaseSRWLockExclusive(&g_traceMu);
        return true;
    }

    void TraceEvent(void* objp, void* fnp) {
        const auto* fn = static_cast<const UFunction*>(fnp);
        const auto* obj = static_cast<const UObject*>(objp);
        if (!PtrOk(fn)) return;
        AcquireSRWLockExclusive(&g_traceMu);
        auto m = g_match.find(uintptr_t(fn));
        if (m == g_match.end()) {
            const std::string name = (PtrOk(fn->Outer) ? fn->Outer->GetName() : "?") + "::" + fn->GetName();
            m = g_match.emplace(uintptr_t(fn), Match(g_pattern, name)).first;
        }
        if (m->second && g_hits.size() < 2000) {
            const UObject* cls = PtrOk(obj) && PtrOk(obj->Class) ? obj->Class : nullptr;
            Hit& h = g_hits[uintptr_t(fn) * 31 + uintptr_t(cls)];
            const double t = (GetTickCount64() - g_t0) / 1000.0;
            if (!h.count) {
                h.fn = (PtrOk(fn->Outer) ? fn->Outer->GetName() : "?") + "::" + fn->GetName();
                h.on = cls ? cls->GetName() : "?";
                h.first = t;
            }
            h.count++;
            h.last = t;
        }
        ReleaseSRWLockExclusive(&g_traceMu);
    }

    std::string TraceEnd(double seconds) {
        AcquireSRWLockExclusive(&g_traceMu);
        std::vector<const Hit*> v;
        for (const auto& [k, h] : g_hits) v.push_back(&h);
        std::sort(v.begin(), v.end(), [](const Hit* a, const Hit* b) { return a->count > b->count; });
        std::string s = "{\"pattern\":" + reflect::Esc(g_pattern) + ",\"seconds\":" + F(seconds) + ",\"functions_seen\":"
                      + std::to_string(g_match.size()) + ",\"hits\":[";
        for (size_t i = 0; i < v.size() && i < 300; i++)
            s += (i ? "," : "") + std::string("{\"fn\":") + reflect::Esc(v[i]->fn) + ",\"on\":" + reflect::Esc(v[i]->on) + ",\"count\":"
               + std::to_string(v[i]->count) + ",\"first_s\":" + F(v[i]->first) + ",\"last_s\":" + F(v[i]->last) + "}";
        s += "]}";
        g_match.clear();
        g_hits.clear();
        ReleaseSRWLockExclusive(&g_traceMu);
        return Ok(s);
    }
}

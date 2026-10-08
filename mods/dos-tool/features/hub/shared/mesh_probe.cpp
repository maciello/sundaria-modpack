#include "mesh_probe.hpp"
#include "ref.hpp"
#include "logger.hpp"

#include <Windows.h>
#include <algorithm>
#include <cctype>
#include <cstdio>
#include <string>
#include <unordered_set>
#include <vector>
#include "Engine_classes.hpp"

using namespace SDK;

namespace {
    bool PtrOk(const void* p) {
        const uintptr_t v = reinterpret_cast<uintptr_t>(p);
        return v > 0x10000 && v < 0x7FFFFFFFFFFFull;
    }

    std::string Lower(std::string s) {
        std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return char(std::tolower(c)); });
        return s;
    }

    std::vector<std::string> ReadTerms() {
        char path[MAX_PATH] = {};
        GetModuleFileNameA(nullptr, path, MAX_PATH);
        std::string p = path;
        p = p.substr(0, p.find_last_of("\/") + 1) + "dos-tool-meshes.txt";
        std::vector<std::string> terms;
        HANDLE h = CreateFileA(p.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
        if (h == INVALID_HANDLE_VALUE) return terms;
        char buf[4096] = {};
        DWORD n = 0;
        ReadFile(h, buf, sizeof(buf) - 1, &n, nullptr);
        CloseHandle(h);
        std::string line;
        for (char c : std::string(buf, n) + "\n") {
            if (c == '\n' || c == '\r') { if (!line.empty()) terms.push_back(Lower(line)); line.clear(); }
            else line += c;
        }
        return terms;
    }

    std::unordered_set<ref::Ref, ref::Hash> g_logged;

    // The allowlisted GObjects walk (scripts/gobjects-check.sh): dev probe, file trigger, once per level load.
    void MeshReport(const std::vector<std::string>& terms, const std::string& world) {
        const ULONGLONG t0 = GetTickCount64();
        UClass* meshCls = UStaticMesh::StaticClass();
        int matches = 0, fresh = 0, meshes = 0;
        char buf[512];
        for (int i = 0; i < UObject::GObjects->Num(); i++) {
            UObject* o = UObject::GObjects->GetByIndex(i);
            if (!PtrOk(o) || !PtrOk(o->Class) || !o->IsA(meshCls)) continue;
            meshes++;
            const std::string low = Lower(o->GetName());
            bool match = false;
            for (const std::string& t : terms) match |= low.find(t) != std::string::npos;
            if (!match) continue;
            matches++;
            if (!g_logged.insert(ref::Ref(o)).second) continue;
            fresh++;
            int simple = -1, trace = -1;
            if (UBodySetup* bs = static_cast<UStaticMesh*>(o)->BodySetup; PtrOk(bs)) {
                const FKAggregateGeom& g = bs->AggGeom;
                simple = g.SphereElems.Num() + g.BoxElems.Num() + g.SphylElems.Num() + g.ConvexElems.Num() + g.TaperedCapsuleElems.Num();
                trace = int(bs->CollisionTraceFlag);
            }
            std::string pkg = "?";  // outermost outer = the package (/Game/...): the path LoadAsset needs
            for (UObject* p = o->Outer; PtrOk(p); p = p->Outer) pkg = p->Name.GetRawString();  // GetName() drops the folders
            std::snprintf(buf, sizeof(buf), "[meshes] %s.%s  simple=%d trace=%d%s", pkg.c_str(), o->GetName().c_str(), simple, trace,
                          simple > 0 ? "  <-- HAS COLLISION" : "");
            logger::log(buf);
        }
        std::snprintf(buf, sizeof(buf), "[meshes] world %s: %d static meshes loaded, %d matching, %d new (%llu ms)", world.c_str(), meshes, matches, fresh,
                      GetTickCount64() - t0);
        logger::log(buf);
    }
}

void mesh_probe::Tick() {
    static ULONGLONG next = 0;
    static ref::Ref seen;
    const ULONGLONG now = GetTickCount64();
    if (now < next) return;
    next = now + 2000;
    UWorld* w = UWorld::GetWorld();
    if (!PtrOk(w) || seen.Is(w)) return;
    const std::vector<std::string> terms = ReadTerms();
    if (terms.empty()) return;
    seen = ref::Ref(w);
    MeshReport(terms, w->GetName());
}

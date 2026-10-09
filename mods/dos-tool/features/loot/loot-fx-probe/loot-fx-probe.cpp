#include "feature.hpp"
#include "game.hpp"
#include "logger.hpp"
#include "ref.hpp"
#include "tmap.hpp"
#include "umg.hpp"

#include <Windows.h>
#include <algorithm>
#include <atomic>
#include <cstdarg>
#include <map>
#include <set>
#include <string>
#include "Engine_classes.hpp"
#include "Niagara_classes.hpp"

// Loot FX probe (dev, #27; file trigger loot-fx.probe next to the exe -> dos-tool-loot-fx.yaml). Game thread.
// What the game itself can show on loot: every loaded Cascade/Niagara system with its instance (colour) parameters,
// loot/rim/rarity materials, the live FX components in the world by owner class, the actor classes the game spawned
// (ReceiveBeginPlay) since the probe was switched on, and the highlight/loot enums.
using namespace SDK;
using umg::PtrOk;

namespace {
    std::atomic<bool> g_probe{false};
    bool g_listening = false;
    std::map<std::string, int> g_begun;  // game thread: actor class -> ReceiveBeginPlay count since the probe went on
    ref::Fn g_beginPlay{AActor::StaticClass, "Actor", "ReceiveBeginPlay"};

    std::string F(const char* fmt, ...) {
        char b[1024];
        va_list a;
        va_start(a, fmt);
        std::vsnprintf(b, sizeof b, fmt, a);
        va_end(a);
        return b;
    }

    std::string ExeDir() {
        char buf[MAX_PATH] = {};
        GetModuleFileNameA(nullptr, buf, MAX_PATH);
        const std::string p = buf;
        return p.substr(0, p.find_last_of("\\/") + 1);
    }

    std::string Lower(std::string s) {
        std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return char(std::tolower(c)); });
        return s;
    }
    bool Matches(const std::string& s) {
        static const char* words[] = {"loot", "drop", "item", "rarit", "grade", "beam", "glow", "sparkl", "shine", "twinkl", "pickup",
                                      "collect", "treasure", "chest", "reward", "rim", "outline", "highlight", "stencil", "gold", "epic",
                                      "legend", "unlock", "pp_"};
        const std::string l = Lower(s);
        return std::any_of(std::begin(words), std::end(words), [&](const char* w) { return l.find(w) != std::string::npos; });
    }

    std::string Path(const UObject* o) {  // Package.Name
        const UObject* top = o;
        while (PtrOk(top->Outer)) top = top->Outer;
        return top == o ? o->GetName() : top->GetName() + "." + o->GetName();
    }
    std::string ClassOf(const UObject* o) { return PtrOk(o) && PtrOk(o->Class) ? o->Class->GetName() : "-"; }

    template <class T> const T* OuterOf(const UObject* o) {
        for (const UObject* p = o; PtrOk(p); p = p->Outer)
            if (p->IsA(T::StaticClass())) return static_cast<const T*>(p);
        return nullptr;
    }

    std::vector<std::string> EnumNames(const char* name) {  // BP enums dump as NewEnumeratorN; the UUserDefinedEnum keeps display names
        std::vector<std::string> out;
        UEnum* e = UObject::FindObjectFast<UEnum>(name, EClassCastFlags::Enum);
        if (!PtrOk(e)) return out;
        std::map<std::string, std::string> display;
        if (e->IsA(UUserDefinedEnum::StaticClass()))
            tmap::ForEach(static_cast<UUserDefinedEnum*>(e)->DisplayNameMap, [&](const FName& k, const FText& t) {
                if (PtrOk(t.TextData)) display[k.ToString()] = t.ToString();
            });
        for (int i = 0; i < e->Names.Num(); i++) {
            std::string n = e->Names[i].Key().ToString();
            if (auto p = n.rfind("::"); p != std::string::npos) n = n.substr(p + 2);
            const int64 v = e->Names[i].Value();
            if (v < 0 || v > 255 || n.ends_with("_MAX")) continue;
            if (int(out.size()) <= v) out.resize(v + 1);
            auto it = display.find(n);
            out[v] = it != display.end() && !it->second.empty() ? it->second : n;
        }
        return out;
    }

    std::string Report() {
        UWorld* world = UWorld::GetWorld();
        struct Fx { std::string kind; std::set<std::string> params; };
        std::map<std::string, Fx> systems;                 // path -> kind + parameter names
        std::set<std::string> materials;
        std::map<std::string, int> comps;                  // "owner class | component | template | active" -> count
        for (int i = 0; UObject::GObjects && i < UObject::GObjects->Num(); i++) {
            UObject* o = UObject::GObjects->GetByIndex(i);
            if (!PtrOk(o) || !PtrOk(o->Class) || o->IsDefaultObject()) continue;
            if (o->IsA(UParticleSystem::StaticClass())) {
                systems[Path(o)].kind = "cascade";
            } else if (o->IsA(UNiagaraSystem::StaticClass())) {
                Fx& fx = systems[Path(o)];
                fx.kind = "niagara";
                const auto& user = static_cast<UNiagaraSystem*>(o)->ExposedParameters.SortedParameterOffsets;
                for (int k = 0; k < user.Num(); k++) fx.params.insert(user[k].Name.ToString());
            } else if (o->IsA(UDistributionVectorParticleParameter::StaticClass()) || o->IsA(UDistributionFloatParticleParameter::StaticClass())) {
                const UObject* ps = OuterOf<UParticleSystem>(o);
                if (!ps) continue;
                const bool vec = o->IsA(UDistributionVectorParticleParameter::StaticClass());
                const FName& n = vec ? static_cast<UDistributionVectorParticleParameter*>(o)->ParameterName
                                     : static_cast<UDistributionFloatParticleParameter*>(o)->ParameterName;
                systems[Path(ps)].params.insert(n.ToString() + (vec ? ":vector" : ":float"));
            } else if (o->IsA(UMaterialInterface::StaticClass())) {
                const std::string p = Path(o);
                if (Matches(p)) materials.insert(ClassOf(o) + " " + p);
            } else if (o->IsA(UFXSystemComponent::StaticClass())) {
                if (!PtrOk(world) || OuterOf<UWorld>(o) != world) continue;
                const AActor* owner = OuterOf<AActor>(o);
                const UObject* tpl = o->IsA(UParticleSystemComponent::StaticClass()) ? static_cast<UObject*>(static_cast<UParticleSystemComponent*>(o)->Template)
                                   : o->IsA(UNiagaraComponent::StaticClass())       ? static_cast<UObject*>(static_cast<UNiagaraComponent*>(o)->Asset)
                                                                                    : nullptr;
                const auto* c = static_cast<UActorComponent*>(o);
                comps[F("%s | %s | %s | active=%d", owner ? ClassOf(owner).c_str() : "-", o->GetName().c_str(), PtrOk(tpl) ? Path(tpl).c_str() : "-",
                        int(c->bIsActive))]++;
            }
        }

        std::string y = F("world: %s\n", PtrOk(world) ? world->GetName().c_str() : "-");
        y += "enums:\n";
        for (const char* en : {"EHighlightStencil", "ELootLevelType", "ELootActivationOption", "EItemGrade"}) {
            const auto n = EnumNames(en);
            y += F("  %s: [", en);
            for (size_t i = 0; i < n.size(); i++) y += F("%s\"%s\"", i ? ", " : "", n[i].c_str());
            y += "]\n";
        }
        y += "fx_systems:  # every loaded system; match = name hints at loot/rarity/glow; params = instance/user parameters\n";
        for (const auto& [p, fx] : systems) {
            y += F("  - {kind: %s, match: %d, path: \"%s\", params: [", fx.kind.c_str(), int(Matches(p)), p.c_str());
            bool first = true;
            for (const auto& n : fx.params) { y += F("%s\"%s\"", first ? "" : ", ", n.c_str()); first = false; }
            y += "]}\n";
        }
        y += "materials:  # loaded materials whose path hints at loot/rim/rarity/highlight\n";
        for (const auto& m : materials) y += F("  - \"%s\"\n", m.c_str());
        y += "fx_components:  # live in this world: owner class | component | template | active -> count\n";
        for (const auto& [k, n] : comps) y += F("  - {n: %d, c: \"%s\"}\n", n, k.c_str());
        y += "begin_play_classes:  # actor classes the game spawned/streamed in since the probe went on -> count\n";
        for (const auto& [k, n] : g_begun) y += F("  \"%s\": %d\n", k.c_str(), n);
        y += F("counts: {systems: %d, materials: %d, components: %d}\n", int(systems.size()), int(materials.size()), int(comps.size()));
        return y;
    }

    void Write() {
        const std::string out = Report();
        HANDLE h = CreateFileA((ExeDir() + "dos-tool-loot-fx.yaml").c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h == INVALID_HANDLE_VALUE) return;
        DWORD w = 0;
        WriteFile(h, out.data(), DWORD(out.size()), &w, nullptr);
        CloseHandle(h);
        logger::log("[loot-fx-probe] " + std::to_string(out.size()) + " bytes -> dos-tool-loot-fx.yaml");
    }

    void OnEvent(void* objp, void* fnp, void*) {
        if (!game::OnGameThread() || !PtrOk(objp) || !PtrOk(fnp)) return;
        auto* obj = static_cast<UObject*>(objp);
        const auto* fn = static_cast<const UFunction*>(fnp);
        // Overrides of ReceiveBeginPlay are separate UFunctions with the same name: compare names, not pointers.
        if (const UFunction* bp = g_beginPlay.Get(); bp && fn->Name == bp->Name && obj->IsA(AActor::StaticClass())) {
            g_begun[ClassOf(obj)]++;
            return;
        }
        if (umg::IsWorldTick(fnp) && g_probe.exchange(false)) Write();
    }

    // ReceiveBeginPlay of every class (each override is its own UFunction, same name) + the world tick.
    void Listen(bool on) {
        game::On(nullptr, "ReceiveBeginPlay", &OnEvent, on);
        game::OnWorldTick(&OnEvent, on);
    }

    struct LootFxProbe : feature::Feature {
        double next = 0;
        std::string path;
        LootFxProbe() : Feature("Loot FX probe", feature::Stage::Alpha) { optIn = true; }
        void OnFrame(const feature::Frame& f) override {
            if (!g_listening) Listen(g_listening = true);
            if (f.now < next) return;
            next = f.now + 1.0;
            if (path.empty()) path = ExeDir() + "loot-fx.probe";
            if (GetFileAttributesA(path.c_str()) == INVALID_FILE_ATTRIBUTES) return;
            DeleteFileA(path.c_str());
            g_probe = true;
        }
        void Off() override {
            Listen(g_listening = false);
            g_probe = false;
        }
    } g_loot_fx_probe;
}

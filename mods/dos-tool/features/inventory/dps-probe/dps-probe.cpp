#include "feature.hpp"
#include "game.hpp"
#include "logger.hpp"
#include "reflect.hpp"
#include "umg.hpp"
#include "../shared/sdk.hpp"

#include <Windows.h>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdarg>
#include <string>
#include <vector>
#include "Engine_classes.hpp"
#include "Archon_classes.hpp"
#include "GameplayAbilities_classes.hpp"

// DPS probe (dev, #89; file trigger dps-probe.probe next to the exe -> dos-tool-dps-state.yaml). Game thread.
// Live damage inputs only: the hero's attribute sets, equipped items with their rolled stats, heroism, and the
// nearest enemies' attribute sets. Static game data (tables, formula) comes from the offline pak extractor.
using namespace SDK;
using umg::PtrOk;

namespace {
    std::atomic<bool> g_probe{false};
    bool g_listening = false;
    thread_local bool t_busy = false;

    std::string F(const char* fmt, ...) {
        char b[512];
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

    // Every float property of a set by reflection (no guessed offsets): `{Name: value, ...}`.
    std::string Floats(UObject* set) {
        std::string y = "{";
        for (const FProperty* p : reflect::Props(set->Class))
            if (reflect::Type(p) == "FloatProperty") {
                const float v = *reinterpret_cast<const float*>(reinterpret_cast<const uint8*>(set) + p->Offset);
                y += F("%s%s: %.6g", y.size() > 1 ? ", " : "", reflect::Name(p).c_str(), v);
            }
        return y + "}";
    }

    std::string Sets(AArchonCharacter* c, const char* indent) {
        std::string y;
        UArchonAbilitySystemComponent* asc = c->mAbilitySystemComponent;
        if (!PtrOk(asc)) return y;
        for (int i = 0; i < asc->SpawnedAttributes.Num(); i++) {
            UAttributeSet* s = asc->SpawnedAttributes[i];
            if (PtrOk(s) && PtrOk(s->Class)) y += F("%s%s: ", indent, s->Class->GetName().c_str()) + Floats(s) + "\n";
        }
        return y;
    }

    // Game thread (world tick). Non-player characters of the loaded levels, nearest to `from` first, at most n.
    std::vector<AArchonCharacter*> Enemies(const AArchonCharacter* from, int n, std::vector<float>& dist) {
        std::vector<std::pair<float, AArchonCharacter*>> all;
        UWorld* w = UWorld::GetWorld();
        if (!PtrOk(w) || !PtrOk(from->RootComponent)) return {};
        const FVector p = from->RootComponent->RelativeLocation;
        for (int li = 0; li < w->Levels.Num(); li++) {
            ULevel* lvl = w->Levels[li];
            if (!PtrOk(lvl)) continue;
            for (int ai = 0; ai < lvl->Actors.Num(); ai++) {
                AActor* a = lvl->Actors[ai];
                if (!PtrOk(a) || !PtrOk(a->Class) || a == from || !a->IsA(AArchonCharacter::StaticClass()) || !PtrOk(a->RootComponent)) continue;
                auto* c = static_cast<AArchonCharacter*>(a);
                if (!PtrOk(c->mAbilitySystemComponent) || (PtrOk(c->Controller) && c->Controller->IsA(APlayerController::StaticClass()))) continue;
                const FVector q = a->RootComponent->RelativeLocation;
                all.push_back({float(std::hypot(std::hypot(q.X - p.X, q.Y - p.Y), q.Z - p.Z)), c});
            }
        }
        std::sort(all.begin(), all.end(), [](auto& a, auto& b) { return a.first < b.first; });
        std::vector<AArchonCharacter*> out;
        for (size_t i = 0; i < all.size() && int(i) < n; i++) {
            out.push_back(all[i].second);
            dist.push_back(all[i].first);
        }
        return out;
    }

    std::string Report() {
        std::string y = F("tick_ms: %llu\n", GetTickCount64());
        APlayerController* pc = items::sdk::LocalPC();
        auto* hero = PtrOk(pc) && PtrOk(pc->Pawn) && pc->Pawn->IsA(AArchonCharacter::StaticClass()) ? static_cast<AArchonCharacter*>(pc->Pawn) : nullptr;
        if (!hero) return y + "hero: null  # no AArchonCharacter pawn\n";
        y += F("hero:\n  class: %s\n  attribute_sets:\n", hero->Class->GetName().c_str()) + Sets(hero, "    ");

        y += "  equipped:  # name, grade, level, rolled stats (non-zero Secondary attributes)\n";
        const items::io::Located w = items::io::Locate();
        if (!items::io::Ready()) y += "    # item names not loaded yet (items::io::Tick), retry\n";
        else
            for (const items::Item& it : items::io::Read(w.bag, false, true)) {
                if (it.where != items::Where::Equipped) continue;
                std::string st;
                for (const items::Stat& s : it.stats) st += F("%s%s: %.6g", st.empty() ? "" : ", ", items::StatName(items::io::GetNames().stat, s.type).c_str(), s.value);
                y += F("    - {name: \"%s\", spec: %d, slot: %d, kind: %s, type: \"%s\", grade: %d, level: %d, stats: {%s}}\n", it.name.c_str(), it.specId,
                       it.equipSlot, items::kKindName[int(it.kind)], it.typeName.c_str(), it.grade, it.level, st.c_str());
            }

        std::vector<float> dist;
        y += "enemies:  # nearest non-player characters\n";
        const std::vector<AArchonCharacter*> foes = Enemies(hero, 3, dist);
        for (size_t i = 0; i < foes.size(); i++)
            y += F("  - class: %s\n    name: %s\n    distance: %.1f\n    attribute_sets:\n", foes[i]->Class->GetName().c_str(), foes[i]->GetName().c_str(),
                   dist[i]) + Sets(foes[i], "      ");
        return y;
    }

    void Write() {
        const std::string out = Report();
        HANDLE h = CreateFileA((ExeDir() + "dos-tool-dps-state.yaml").c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h == INVALID_HANDLE_VALUE) return;
        DWORD wr = 0;
        WriteFile(h, out.data(), DWORD(out.size()), &wr, nullptr);
        CloseHandle(h);
        logger::log("[dps-probe] " + std::to_string(out.size()) + " bytes -> dos-tool-dps-state.yaml");
    }

    void OnEvent(void*, void* fn, void*) {
        if (t_busy || !game::OnGameThread() || !umg::IsWorldTick(fn)) return;
        t_busy = true;
        if (g_probe.exchange(false)) Write();
        t_busy = false;
    }

    struct DpsProbe : feature::Feature {
        double next = 0;
        std::string path;
        DpsProbe() : Feature("DPS probe", feature::Stage::Alpha) { optIn = true; }
        void OnFrame(const feature::Frame& f) override {
            if (!g_listening) game::SetEventListener(&OnEvent, g_listening = true);
            items::io::Tick();
            if (f.now < next) return;
            next = f.now + 1.0;
            if (path.empty()) path = ExeDir() + "dps-probe.probe";
            if (GetFileAttributesA(path.c_str()) == INVALID_FILE_ATTRIBUTES) return;
            DeleteFileA(path.c_str());
            g_probe = true;
        }
    } g_dps_probe;
}

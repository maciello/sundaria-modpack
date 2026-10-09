#include "feature.hpp"
#include "ecs.hpp"
#include "ref.hpp"
#include "script.hpp"
#include "imgui.h"
#include "imgui_internal.h"  // MarkIniSettingsDirty

#include <Windows.h>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string>
#include <unordered_map>
#include <vector>
#include "Engine_classes.hpp"
#include "Archon_classes.hpp"
#include "GameplayAbilities_classes.hpp"
#include "GameplayAbilities_parameters.hpp"
#include "BP_GameplayAnimNotify_classes.hpp"
#include "BP_GameAbilityBase_classes.hpp"

// Ability mods: Lua scripts in <Win64>/dos-mods/abilities/*.lua tweak existing abilities and build new
// ones on donors (script.hpp has the API). Scripts only declare and record commands; the systems below
// run on the game thread (core's ProcessEvent listener) and are the only code that touches the game:
//   cast tracking  -> activate / end events, anim rate, cooldown watch
//   notify         -> out event (the ability's ApplyEffect / ShootProjectile frame)
//   cooldown watch -> cooldown effects of that cast become timers removed after duration x scale
//   commands       -> dash, apply_effect, play_rate, cancel
// State is ECS entities (ecs.hpp). Local player only; host-side in co-op (#41).
using namespace SDK;
using ability_script::Command;
using ability_script::Event;

namespace {
    inline bool PtrOk(const void* p) {
        const uintptr_t v = reinterpret_cast<uintptr_t>(p);
        return v > 0x10000 && v < 0x7FFFFFFFFFFFull;
    }

    // Components.
    struct Cast { ref::Ref ability, montage; std::string cls; bool out = false; };  // UGameplayAbility, UAnimMontage
    struct ProjectileSwap { ref::Ref ability, original; };  // UBP_GameAbilityBase_C, UClass; on the cast entity, undone at its end
    struct CooldownWatch { ref::Ref asc, ability; float scale; ULONGLONG until; std::vector<int> seen; };
    struct CooldownTimer { ref::Ref asc; int handle; ULONGLONG at; };

    // Game thread state (Off() touches it only after the listener has drained).
    ecs::Registry g_reg;
    ability_script::Host g_host;
    std::string g_dir, g_stampMemo;
    std::vector<Command> g_cmds;
    std::unordered_map<std::string, ref::Ref> g_effectClasses;  // name -> UClass (ptr null = not found), FindClassFast is slow
    ref::Ref g_lastAbility, g_lastMontage;
    ULONGLONG g_lastTick = 0, g_lastScan = 0;
    thread_local bool t_busy = false;  // our own UFunction calls re-enter ProcessEvent

    std::atomic<bool> g_on{false}, g_reload{false};
    std::atomic<int> g_casts{0}, g_outs{0}, g_cuts{0}, g_commands{0}, g_reloads{0};
    std::atomic<float> g_liveRate{0};

    // Game thread. Blueprint classes come and go with the map (#63): ref::Fn re-resolves them.
    ref::Fn g_fnNotify{UBP_GameplayAnimNotify_C::StaticClass, "BP_GameplayAnimNotify_C", "Received_Notify"};
    ref::Fn g_fnCancel{UGameplayAbility::StaticClass, "GameplayAbility", "K2_CancelAbility"};
    ref::Fn g_fnMakeContext{UAbilitySystemComponent::StaticClass, "AbilitySystemComponent", "MakeEffectContext"};
    ref::Fn g_fnApplySelf{UAbilitySystemComponent::StaticClass, "AbilitySystemComponent", "BP_ApplyGameplayEffectToSelf"};
    std::atomic<bool> g_ready{false};  // the notify class was found once (menu)

    // Menu snapshot, written on the game thread, read on the render thread.
    SRWLOCK g_mu = SRWLOCK_INIT;
    struct NewRow { std::string name, donor, file; bool active; };
    struct Snapshot {
        int errors = 0, decls = 0;
        std::vector<NewRow> news;
        std::string lastCast;
    } g_snap;
    std::vector<std::pair<std::string, bool>> g_pendingActive;  // menu -> game thread
    std::vector<std::string> g_off;                            // saved in dos-tool.ini

    void Call(const UObject* obj, UFunction* fn, void* parms) {
        if (!fn) return;
        auto flags = fn->FunctionFlags;
        fn->FunctionFlags |= 0x400;  // FUNC_Native
        obj->ProcessEvent(fn, parms);
        fn->FunctionFlags = flags;
    }

    AArchonCharacter* LocalHero() {
        UWorld* w = UWorld::GetWorld();
        if (!PtrOk(w) || !PtrOk(w->OwningGameInstance)) return nullptr;
        auto& lps = w->OwningGameInstance->LocalPlayers;
        if (lps.Num() <= 0 || !PtrOk(lps[0])) return nullptr;
        APlayerController* pc = lps[0]->PlayerController;
        if (!PtrOk(pc) || !PtrOk(pc->Pawn) || !pc->Pawn->IsA(AArchonCharacter::StaticClass())) return nullptr;
        return static_cast<AArchonCharacter*>(pc->Pawn);
    }

    std::string ScriptDir() {
        char exe[MAX_PATH];
        GetModuleFileNameA(nullptr, exe, MAX_PATH);
        if (char* slash = strrchr(exe, '\\')) slash[1] = 0;
        return std::string(exe) + "dos-mods\\abilities\\";
    }

    // Reloads the scripts when a file changed (checked once a second) or the menu asked.
    void Scan(ULONGLONG now) {
        if (now - g_lastScan < 1000 && !g_reload) return;
        g_lastScan = now;
        std::vector<ability_script::Stamp> stamps;
        WIN32_FIND_DATAA fd;
        HANDLE h = FindFirstFileA((g_dir + "*.lua").c_str(), &fd);
        if (h != INVALID_HANDLE_VALUE) {
            do {
                if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
                stamps.push_back({fd.cFileName, (unsigned long long)fd.ftLastWriteTime.dwHighDateTime << 32 | fd.ftLastWriteTime.dwLowDateTime,
                                  (unsigned long long)fd.nFileSizeHigh << 32 | fd.nFileSizeLow});
            } while (FindNextFileA(h, &fd));
            FindClose(h);
        }
        std::sort(stamps.begin(), stamps.end(), [](auto& a, auto& b) { return a.name < b.name; });
        const bool forced = g_reload.exchange(false);
        if (!ability_script::Changed(stamps, g_stampMemo) && !forced) return;
        std::vector<std::pair<std::string, std::string>> files;
        for (const auto& s : stamps) {
            if (!ability_script::Wanted(s.name)) continue;
            std::string src;
            if (FILE* f = fopen((g_dir + s.name).c_str(), "rb")) {
                char buf[4096];
                for (size_t n; (n = fread(buf, 1, sizeof(buf), f)) > 0;) src.append(buf, n);
                fclose(f);
            }
            files.emplace_back(s.name, std::move(src));
        }
        g_host.Load(files);
        g_reloads++;
    }

    // Errors, the script log and what loaded go to dos-mods/abilities/log.txt (the Insert menu holds flags and
    // tuning only, .claude/rules/design.md); the menu gets counts and the new-ability toggles.
    void Publish() {
        Snapshot s;
        s.errors = static_cast<int>(g_host.Errors().size());
        s.decls = static_cast<int>(g_host.Rules().size() + g_host.Hooks().size());
        for (const auto& n : g_host.News()) s.news.push_back({n.name, n.donor, n.file, g_host.Active(n.name)});
        if (FILE* f = fopen((g_dir + "log.txt").c_str(), "wb")) {
            fprintf(f, "Ability mods (written by DoS-Tool on every change; not a script)\r\n\r\n== Errors\r\n");
            for (const auto& e : g_host.Errors()) fprintf(f, "%s%s%s\r\n", e.msg.find(e.file) == 0 ? "" : e.file.c_str(),
                                                         e.msg.find(e.file) == 0 ? "" : ": ", e.msg.c_str());
            fprintf(f, "\r\n== Loaded\r\n");
            for (const auto& r : g_host.Rules()) {
                fprintf(f, "%s: tweak %s", r.file.c_str(), r.pattern.c_str());
                if (r.tweak.hasRate) fprintf(f, "  anim_rate %.2f", r.tweak.animRate);
                if (r.tweak.hasCooldown) fprintf(f, "  cooldown %.2f", r.tweak.cooldown);
                if (!r.tweak.projectile.empty()) fprintf(f, "  projectile %s", r.tweak.projectile.c_str());
                fprintf(f, "\r\n");
            }
            for (const auto& hk : g_host.Hooks())
                fprintf(f, "%s: on %s %s\r\n", hk.file.c_str(), hk.pattern.c_str(), ability_script::kEventNames[int(hk.ev)]);
            for (const auto& n : g_host.News())
                fprintf(f, "%s: new '%s' on %s%s\r\n", n.file.c_str(), n.name.c_str(), n.donor.c_str(), g_host.Active(n.name) ? "" : " (off)");
            fprintf(f, "\r\n== Log\r\n");
            for (const auto& l : g_host.Log()) fprintf(f, "%s\r\n", l.c_str());
            fclose(f);
        }
        AcquireSRWLockExclusive(&g_mu);
        s.lastCast = std::move(g_snap.lastCast);
        g_snap = std::move(s);
        ReleaseSRWLockExclusive(&g_mu);
    }

    void SetRate(AArchonCharacter* hero, UAnimMontage* montage, float rate) {
        if (!PtrOk(montage) || !PtrOk(hero->Mesh)) return;
        UAnimInstance* anim = hero->Mesh->GetAnimInstance();
        if (!PtrOk(anim)) return;
        anim->Montage_SetPlayRate(montage, rate);
        g_liveRate = anim->Montage_GetPlayRate(montage);
    }

    // Loaded classes only (FindClassFast walks GObjects: cached per name, misses too).
    std::unordered_map<std::string, ref::Ref> g_projectileClasses;
    UClass* ProjectileClass(const std::string& name) {
        auto it = g_projectileClasses.find(name);
        if (it != g_projectileClasses.end() && (!it->second.ptr || it->second.Get())) return it->second.Get<UClass>();  // hit or known miss
        UClass* cls = UObject::FindClassFast(name);
        if (!PtrOk(cls) && !name.ends_with("_C")) cls = UObject::FindClassFast(name + "_C");
        if (PtrOk(cls) && !cls->IsSubclassOf(AActor::StaticClass())) cls = nullptr;
        g_projectileClasses[name] = ref::Ref(cls);
        return PtrOk(cls) ? cls : nullptr;
    }

    void UndoSwap(ecs::Entity e) {
        // the original class may have been collected while swapped out (our write hid its reference): then none
        if (ProjectileSwap* s = g_reg.Get<ProjectileSwap>(e))
            if (auto* ab = s->ability.Get<UBP_GameAbilityBase_C>()) ab->mProjectileClass = s->original.Get<UClass>();
    }

    UClass* EffectClass(const std::string& name) {
        auto it = g_effectClasses.find(name);
        if (it != g_effectClasses.end() && (!it->second.ptr || it->second.Get())) return it->second.Get<UClass>();  // hit or known miss
        UClass* cls = UObject::FindClassFast(name);
        if (!PtrOk(cls) && !name.ends_with("_C")) cls = UObject::FindClassFast(name + "_C");
        if (PtrOk(cls) && !cls->IsSubclassOf(UGameplayEffect::StaticClass())) cls = nullptr;
        g_effectClasses[name] = ref::Ref(cls);
        return PtrOk(cls) ? cls : nullptr;
    }

    void Run(AArchonCharacter* hero, UAbilitySystemComponent* asc, const Cast* cast) {
        std::vector<Command> cmds;
        cmds.swap(g_cmds);
        for (const Command& c : cmds) {
            g_commands++;
            switch (c.kind) {
                case Command::Kind::Dash: {
                    const float yaw = hero->K2_GetActorRotation().Yaw * 3.14159265f / 180.0f;
                    hero->LaunchCharacter(FVector{std::cos(yaw) * c.value, std::sin(yaw) * c.value, 0}, true, false);
                    break;
                }
                case Command::Kind::PlayRate:
                    if (cast) SetRate(hero, cast->montage.Get<UAnimMontage>(), c.value);
                    break;
                case Command::Kind::Cancel:
                    if (auto* ab = cast ? cast->ability.Get<UGameplayAbility>() : nullptr) Call(ab, g_fnCancel.Get(), nullptr);
                    break;
                case Command::Kind::ApplyEffect: {
                    UClass* cls = EffectClass(c.text);
                    if (!cls) { g_host.Report("apply_effect", "no GameplayEffect class '" + c.text + "'"); break; }
                    Params::AbilitySystemComponent_MakeEffectContext ctx{};
                    Call(asc, g_fnMakeContext.Get(), &ctx);
                    Params::AbilitySystemComponent_BP_ApplyGameplayEffectToSelf p{};
                    p.GameplayEffectClass = cls;
                    p.Level = 1.0f;
                    p.EffectContext = ctx.ReturnValue;
                    Call(asc, g_fnApplySelf.Get(), &p);
                    break;
                }
            }
        }
    }

    Cast* CurrentCast(ecs::Entity* out = nullptr) {
        Cast* found = nullptr;
        g_reg.Each<Cast>([&](ecs::Entity e, Cast& c) { found = &c; if (out) *out = e; });
        return found;
    }

    // Cast tracking: the local hero's animating ability (or its montage) changed.
    void TrackCast(AArchonCharacter* hero, UAbilitySystemComponent* asc, ULONGLONG now) {
        UGameplayAbility* ab = asc->LocalAnimMontageInfo.AnimatingAbility;
        UAnimMontage* montage = asc->LocalAnimMontageInfo.AnimMontage;
        if (!PtrOk(ab)) ab = nullptr;
        if (!PtrOk(montage)) montage = nullptr;
        auto same = [](const ref::Ref& r, const void* o) { return o ? r.Is(o) : !r.ptr; };
        if (same(g_lastAbility, ab) && same(g_lastMontage, montage)) return;
        g_lastAbility = ref::Ref(ab);
        g_lastMontage = ref::Ref(montage);

        ecs::Entity e;
        if (Cast* old = CurrentCast(&e)) {
            g_host.Fire(old->cls, Event::End, g_cmds);
            UndoSwap(e);
            g_reg.Destroy(e);
        }
        if (!ab) return;
        g_casts++;
        e = g_reg.Create();
        Cast& c = g_reg.Add(e, Cast{ref::Ref(ab), ref::Ref(montage), ab->Class->GetName()});
        const ability_script::Tweak t = g_host.TweakFor(c.cls);
        if (t.hasRate) SetRate(hero, montage, t.animRate);
        if (t.hasCooldown) g_reg.Add(g_reg.Create(), CooldownWatch{ref::Ref(asc), ref::Ref(ab), t.cooldown, now + 3000});
        // Projectile swap: the ability shoots mProjectileClass (unverified that spawning reads it, #44).
        if (!t.projectile.empty() && ab->IsA(UBP_GameAbilityBase_C::StaticClass())) {
            auto* gab = static_cast<UBP_GameAbilityBase_C*>(ab);
            if (UClass* p = ProjectileClass(t.projectile)) {
                g_reg.Add(e, ProjectileSwap{ref::Ref(gab), ref::Ref(gab->mProjectileClass)});
                gab->mProjectileClass = p;
            } else g_host.Report("projectile", "no loaded actor class '" + t.projectile + "'");
        }
        const ability_script::Host::New* n = g_host.NewFor(c.cls);
        AcquireSRWLockExclusive(&g_mu);
        g_snap.lastCast = n ? n->label + " (" + ability_script::ShortName(c.cls) + ")" : ability_script::ShortName(c.cls);
        ReleaseSRWLockExclusive(&g_mu);
        g_host.Fire(c.cls, Event::Activate, g_cmds);
    }

    // Cooldown cut: each cooldown effect of a watched cast becomes a timer at duration x scale.
    void CooldownSystem(ULONGLONG now) {
        g_reg.Each<CooldownWatch>([&](ecs::Entity e, CooldownWatch& w) {
            if (now > w.until) { g_reg.Destroy(e); return; }
            game::EffectRef fx[8];
            const int n = game::CooldownEffects(w.asc.Get(), w.ability.Get(), fx, 8);  // 0 once either is gone
            for (int i = 0; i < n; i++) {
                if (std::find(w.seen.begin(), w.seen.end(), fx[i].handle) != w.seen.end()) continue;
                w.seen.push_back(fx[i].handle);
                if (fx[i].duration <= 0) continue;  // infinite: leave it to the game
                bool timed = false;
                g_reg.Each<CooldownTimer>([&](ecs::Entity, CooldownTimer& t) { timed |= t.handle == fx[i].handle && t.asc == w.asc; });
                if (!timed)
                    g_reg.Add(g_reg.Create(), CooldownTimer{w.asc, fx[i].handle, now + (ULONGLONG)(fx[i].duration * w.scale * 1000.0f)});
            }
        });
        g_reg.Each<CooldownTimer>([&](ecs::Entity e, CooldownTimer& t) {
            if (now < t.at) return;
            if (game::RemoveEffect(t.asc.Get(), t.handle)) g_cuts++;
            g_reg.Destroy(e);
        });
    }

    void OnEvent(void* objp, void* fnp, void* parms) {
        if (t_busy || !g_on.load(std::memory_order_relaxed) || !game::OnGameThread()) return;  // shared state, UFunction calls and ref resolution: game thread only
        UFunction* notifyFn = g_fnNotify.Get();  // O(1); null until a world loads the class
        if (!notifyFn) return;
        g_ready = true;
        const bool notify = fnp == notifyFn;
        const ULONGLONG now = GetTickCount64();
        if (!notify && now - g_lastTick < 15) return;  // systems tick ~60 Hz, notifies at once

        t_busy = true;
        const unsigned version = g_host.Version();
        AcquireSRWLockExclusive(&g_mu);
        std::vector<std::pair<std::string, bool>> pending;
        pending.swap(g_pendingActive);
        ReleaseSRWLockExclusive(&g_mu);
        for (const auto& [name, on] : pending) g_host.SetActive(name, on);

        AArchonCharacter* hero = LocalHero();
        UAbilitySystemComponent* asc = hero ? hero->mAbilitySystemComponent : nullptr;
        if (notify) {
            // Received_Notify(MeshComp, Animation): the effect frame of the animating ability. Trust boundary: the
            // engine's call, so check what it passed (a stale function pointer once gave parms = null, #63).
            auto* n = static_cast<UBP_GameplayAnimNotify_C*>(objp);
            const bool ok = parms && PtrOk(n) && n->IsA(UBP_GameplayAnimNotify_C::StaticClass());
            auto* mesh = ok ? *static_cast<USkeletalMeshComponent**>(parms) : nullptr;
            const auto t = ok ? n->mGameplayAnimNotifyType : EGameplayAnimNotifyType{};
            Cast* c = CurrentCast();
            if (ok && PtrOk(asc) && c && !c->out && mesh == hero->Mesh
                && (t == EGameplayAnimNotifyType::ApplyEffect || t == EGameplayAnimNotifyType::ShootProjectile)
                && c->ability.Is(asc->LocalAnimMontageInfo.AnimatingAbility)) {
                c->out = true;
                g_outs++;
                g_host.Fire(c->cls, Event::Out, g_cmds);
            }
        } else {
            g_lastTick = now;
            Scan(now);
            if (PtrOk(asc)) {
                TrackCast(hero, asc, now);
                CooldownSystem(now);
            }
        }
        if (PtrOk(asc)) Run(hero, asc, CurrentCast());
        else g_cmds.clear();
        if (g_host.Version() != version) Publish();
        t_busy = false;
    }

    // Received_Notify (an ability's effect frame) + the ~60 Hz game tick for the systems.
    void Listen(bool on) {
        game::On("BP_GameplayAnimNotify_C", "Received_Notify", &OnEvent, on);
        game::OnGameTick(&OnEvent, on);
    }

    struct AbilityMods : feature::Feature {
        AbilityMods() : Feature("Ability mods", feature::Stage::Alpha) { optIn = true; }  // new game-thread hook

        void OnFrame(const feature::Frame&) override {
            if (g_on) return;
            g_dir = ScriptDir();
            CreateDirectoryA((g_dir + "..").c_str(), nullptr);  // dos-mods (fails harmlessly if it exists)
            CreateDirectoryA(g_dir.c_str(), nullptr);
            for (const auto& name : g_off) g_host.SetActive(name, false);
            g_stampMemo.clear();
            g_reload = true;
            g_on = true;
            Listen(true);
        }

        // The callbacks have drained when Listen(false) returns: the game-thread state is ours again.
        void Off() override {
            if (!g_on) return;
            g_on = false;
            Listen(false);
            g_reg.Each<ProjectileSwap>([](ecs::Entity e, ProjectileSwap&) { UndoSwap(e); });  // memory writes only
            g_host.Close();  // pending cooldown timers are dropped: those cooldowns run to their vanilla end
            g_reg.Clear();
            g_cmds.clear();
            g_lastAbility = {};
            g_lastMontage = {};
        }

        void Load(const char* key, const char* value) override {
            if (strcmp(key, "off") != 0) return;
            g_off.clear();
            for (const char* p = value; *p;) {
                const char* bar = strchr(p, '|');
                const std::string name = bar ? std::string(p, bar) : std::string(p);
                if (!name.empty()) g_off.push_back(name);
                if (!bar) break;
                p = bar + 1;
            }
        }
        void Save(std::vector<std::pair<std::string, std::string>>& out) override {
            std::string v;
            for (const auto& n : g_off) v += (v.empty() ? "" : "|") + n;
            out.emplace_back("off", v);
        }

        void Menu() override {
            if (!g_ready) { ImGui::TextDisabled("waiting for game classes"); return; }
            ImGui::TextDisabled("scripts: %s", g_dir.c_str());
            if (ImGui::Button("Reload now")) g_reload = true;
            ImGui::SameLine();
            ImGui::TextDisabled("reloads %d  casts %d  out %d  cooldown cuts %d  actions %d", g_reloads.load(), g_casts.load(),
                                g_outs.load(), g_cuts.load(), g_commands.load());

            AcquireSRWLockShared(&g_mu);
            const Snapshot s = g_snap;
            ReleaseSRWLockShared(&g_mu);
            if (!s.lastCast.empty()) ImGui::Text("last cast: %s  anim rate %.2f", s.lastCast.c_str(), g_liveRate.load());

            for (const NewRow& n : s.news) {
                bool active = n.active;
                ImGui::PushID(n.name.c_str());
                if (ImGui::Checkbox("##on", &active)) {
                    AcquireSRWLockExclusive(&g_mu);
                    g_pendingActive.emplace_back(n.name, active);
                    ReleaseSRWLockExclusive(&g_mu);
                    std::erase(g_off, n.name);
                    if (!active) g_off.push_back(n.name);
                    ImGui::MarkIniSettingsDirty();
                }
                ImGui::SameLine();
                ImGui::Text("%s  on %s", n.name.c_str(), n.donor.c_str());
                ImGui::SameLine();
                ImGui::TextDisabled("(%s)", n.file.c_str());
                ImGui::PopID();
            }
            if (s.errors) ImGui::TextWrapped("%d script error(s): see dos-mods/abilities/log.txt", s.errors);
            ImGui::TextDisabled("%d declarations loaded. Examples: _examples.lua (files starting with _ are not loaded).", s.decls + (int)s.news.size());
        }
    } g_ability_mods;
}

#include "feature.hpp"
#include "cast-mode.hpp"
#include "../shared/aim.hpp"
#include "draw.hpp"
#include "logger.hpp"
#include "ref.hpp"
#include "style.hpp"
#include "umg.hpp"
#include "imgui.h"
#include "imgui_internal.h"  // MarkIniSettingsDirty

#include <Windows.h>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <string>
#include "Engine_classes.hpp"
#include "Archon_classes.hpp"
#include "Archon_parameters.hpp"
#include "GameplayAbilities_classes.hpp"
#include "GameplayAbilities_parameters.hpp"
#include "BP_PlayerControllerGame_classes.hpp"
#include "BP_PlayerControllerGame_parameters.hpp"
#include "BP_GameAbilityBase_classes.hpp"
#include "BP_GameAbilityBase_parameters.hpp"
#include "BP_GameState_classes.hpp"
#include "BP_GameState_parameters.hpp"

// Cast mode (#107, marker of #104): pressing an aimed ability starts aiming instead of casting. While aiming, the
// ally/enemy the game's own trace would hit is marked (or a ground circle for area abilities you ticked in the menu);
// left click or the same key casts, right click cancels.
// How: the controller's InpActEvt_Ability<N> key events are filtered before the game runs them (game::SetEventFilter);
// the cast is the game's own EnqueueAbilityInput(slot, press) + (slot, release), so the ability activates and traces
// exactly as if the key had been pressed at that moment. Game thread only, except the drawing (render thread, floats).
using namespace SDK;

namespace {
    bool PtrOk(const void* p) {
        const uintptr_t v = reinterpret_cast<uintptr_t>(p);
        return v > 0x10000 && v < 0x7FFFFFFFFFFFull;
    }

    // Settings: menu (render thread) writes, game thread reads.
    std::atomic<bool> g_on{false}, g_allies{true}, g_enemies{true};
    std::atomic<int> g_groundMask{0};  // bit = EAbilityInputName slot: aim first with a ground circle

    ref::Cached<UClass> g_pcCls{[] { return ABP_PlayerControllerGame_C::StaticClass(); }};
    ref::Fn g_fnBarItem{ABP_PlayerControllerGame_C::StaticClass, "BP_PlayerControllerGame_C", "I_GetActionBarItemByEnum"};
    ref::Fn g_fnEnqueue{ABP_PlayerControllerGame_C::StaticClass, "BP_PlayerControllerGame_C", "EnqueueAbilityInput"};
    ref::Fn g_fnFind{UArchonAbilitySystemComponent::StaticClass, "ArchonAbilitySystemComponent", "FindAbilityFromInputID"};
    ref::Fn g_fnCooldown{UGameplayAbility::StaticClass, "GameplayAbility", "K2_CheckAbilityCooldown"};
    ref::Fn g_fnRange{UBP_GameAbilityBase_C::StaticClass, "BP_GameAbilityBase_C", "GetAbilityTraceRange"};
    ref::Fn g_fnEnemies{ABP_GameState_C::StaticClass, "BP_GameState_C", "I_AreActorsEnemies"};

    // The controller's ability key events: InpActEvt_Ability<N>_* (press and release), n = N, 0 = AbilityBlock.
    struct Input { ref::Ref fn; int n; };
    ref::Ref g_inputFrom;
    Input g_input[40];
    int g_numInput = 0;

    int InputIndex(const void* fn) {
        UClass* pc = g_pcCls.Get();
        if (!pc) return -1;
        if (!(g_inputFrom == g_pcCls.r)) {  // once per class load: O(its functions)
            g_inputFrom = g_pcCls.r;
            g_numInput = 0;
            for (UField* f = pc->Children; PtrOk(f) && g_numInput < 40; f = f->Next) {
                const std::string name = f->GetName();
                constexpr char kPre[] = "InpActEvt_Ability";
                if (!name.starts_with(kPre)) continue;
                const std::string rest = name.substr(sizeof(kPre) - 1);
                if (rest.starts_with("Block_")) g_input[g_numInput++] = {ref::Ref(f), 0};
                else if (std::isdigit(static_cast<unsigned char>(rest[0]))) g_input[g_numInput++] = {ref::Ref(f), std::atoi(rest.c_str())};
            }
        }
        for (int i = 0; i < g_numInput; i++) if (g_input[i].fn.ptr == fn) return i;  // valid: same class as g_inputFrom
        return -1;
    }

    thread_local bool t_busy = false;  // our own calls re-enter ProcessEvent

    AArchonCharacter* Hero(APlayerController* pc) {
        return PtrOk(pc) && PtrOk(pc->Pawn) && pc->Pawn->IsA(AArchonCharacter::StaticClass()) ? static_cast<AArchonCharacter*>(pc->Pawn) : nullptr;
    }

    // What sits in a slot, as far as cast mode cares.
    struct Slot {
        bool aimed = false;
        aim::Kind kind = aim::Kind::None;  // Ally / Enemy = pick a character; None with aimed = ground circle
        ref::Ref ability;
        std::string name;
    };

    Slot Lookup(ABP_PlayerControllerGame_C* pc, int slot) {
        Slot out;
        AArchonCharacter* hero = Hero(pc);
        UFunction* barFn = g_fnBarItem.Get();
        UFunction* findFn = g_fnFind.Get();
        UFunction* cdFn = g_fnCooldown.Get();
        if (!hero || !PtrOk(hero->mAbilitySystemComponent) || !barFn || !findFn || !cdFn) return out;
        Params::BP_PlayerControllerGame_C_I_GetActionBarItemByEnum bar{};
        bar.InputEnum = static_cast<EAbilityInputName>(slot);
        pc->ProcessEvent(barFn, &bar);
        if (!bar.bNonEmpty) return out;
        Params::ArchonAbilitySystemComponent_FindAbilityFromInputID find{};
        find.InputID = bar.ABI.ID_2_B192DADA43F28C263419A7BA69B4B4D2;
        umg::CallNative(hero->mAbilitySystemComponent, findFn, &find);
        UArchonGameplayAbility* ab = find.ReturnValue && PtrOk(find.OutAbility) ? find.OutAbility : nullptr;
        if (!ab || !ab->IsA(UBP_GameAbilityBase_C::StaticClass())) return out;
        auto* base = static_cast<UBP_GameAbilityBase_C*>(ab);
        const int type = int(base->mDefaultAbilityTargetingType);
        const bool pick = aim::Targeted(type, base->mRequireValidTarget) && (type == 2 ? g_allies.load() : g_enemies.load());
        const bool ground = !pick && (g_groundMask.load() >> slot & 1);
        if (!pick && !ground) return out;
        Params::GameplayAbility_K2_CheckAbilityCooldown cd{};
        umg::CallNative(ab, cdFn, &cd);
        if (!cd.ReturnValue) return out;  // on cooldown: the game shows its own message
        out.aimed = true;
        out.kind = pick ? (type == 2 ? aim::Kind::Ally : aim::Kind::Enemy) : aim::Kind::None;
        out.ability = ref::Ref(ab);
        out.name = ab->Class->GetName();
        return out;
    }

    // Game-thread state.
    cast_mode::State g_state;
    Slot g_aim;
    FName g_left, g_right;
    bool g_keysMade = false;
    ULONGLONG g_nextPredict = 0;
    std::atomic<int> g_casts{0}, g_cancels{0};

    // What the render thread draws: plain numbers only.
    struct Shown {
        bool aiming = false;
        aim::Kind want = aim::Kind::None;   // None = ground circle
        bool hasTarget = false;
        float head[3]{}, feet[3]{}, ground[3]{}, radius = 0;
        unsigned serial = 0;                // changes with the target (pop animation)
    };
    SRWLOCK g_mu = SRWLOCK_INIT;
    Shown g_shown;
    ref::Ref g_lastTarget;

    void Publish(const Shown& s) {
        AcquireSRWLockExclusive(&g_mu);
        const unsigned serial = g_shown.serial;
        g_shown = s;
        g_shown.serial = serial + (s.serial ? 1 : 0);
        ReleaseSRWLockExclusive(&g_mu);
    }

    void End(const char* why) {
        g_state.aiming = false;
        g_aim = {};
        g_lastTarget = {};
        Publish({});
        logger::log((std::string("[cast-mode] ") + why).c_str());
    }

    void Cast(ABP_PlayerControllerGame_C* pc, int slot) {
        if (UFunction* fn = g_fnEnqueue.Get()) {
            for (bool press : {true, false}) {  // the game's own press + release of that slot
                Params::BP_PlayerControllerGame_C_EnqueueAbilityInput p{};
                p.Ability = static_cast<EAbilityInputName>(slot);
                p.Press = press;
                pc->ProcessEvent(fn, &p);
            }
            g_casts++;
        }
        End("cast");
    }

    aim::V3 V(const FVector& v) { return {v.X, v.Y, v.Z}; }
    FVector F(aim::V3 v) { return FVector{v.x, v.y, v.z}; }

    // The target the game would pick now (features/targeting/shared/aim.hpp), or the ground point for an area ability.
    void Predict(APlayerController* pc) {
        AArchonCharacter* hero = Hero(pc);
        auto* ab = g_aim.ability.Get<UBP_GameAbilityBase_C>();
        if (!hero || !ab || !PtrOk(pc->PlayerCameraManager)) { End("ability or hero gone"); return; }
        Shown s;
        s.aiming = true;
        s.want = g_aim.kind;

        float range = 0;
        if (UFunction* fn = g_fnRange.Get()) {
            Params::BP_GameAbilityBase_C_GetAbilityTraceRange p{};
            ab->ProcessEvent(fn, &p);
            range = p.Range;
        }
        if (range <= 1.0f) range = 1500.0f;  // no range curve: a sane reach for the circle
        const float radius = ab->TraceSphereRadiusOverride > 0 ? ab->TraceSphereRadiusOverride : 30.0f;
        const aim::V3 cam = V(pc->PlayerCameraManager->GetCameraLocation());
        const FRotator rot = pc->PlayerCameraManager->GetCameraRotation();
        const aim::V3 dir = aim::Dir(rot.Pitch, rot.Yaw), start = V(hero->K2_GetActorLocation());
        const aim::V3 viewEnd = aim::ViewEnd(cam, dir, start, range);

        AActor* ignoreArr[1] = {hero};
        const TArray<AActor*> ignored(ignoreArr, 1, 1);  // non-owning view
        FHitResult view{};
        const FName profile = ab->mTraceProfile.Name;
        const bool viewHit = profile.ComparisonIndex != 0
            ? UKismetSystemLibrary::LineTraceSingleByProfile(hero, F(cam), F(viewEnd), profile, false, ignored, EDrawDebugTrace::None, &view,
                                                             true, FLinearColor{}, FLinearColor{}, 0.0f)
            : UKismetSystemLibrary::LineTraceSingle(hero, F(cam), F(viewEnd), ETraceTypeQuery::TraceTypeQuery1, false, ignored,
                                                    EDrawDebugTrace::None, &view, true, FLinearColor{}, FLinearColor{}, 0.0f);
        const aim::V3 hitAt = V(view.Location);

        if (g_aim.kind == aim::Kind::None) {  // area: where the aim meets the world, dropped onto the floor
            aim::V3 p = viewHit ? hitAt : viewEnd;
            FHitResult floor{};
            if (UKismetSystemLibrary::LineTraceSingle(hero, F(p + aim::V3{0, 0, 50}), F(p - aim::V3{0, 0, 3000}), ETraceTypeQuery::TraceTypeQuery1,
                                                      false, ignored, EDrawDebugTrace::None, &floor, true, FLinearColor{}, FLinearColor{}, 0.0f))
                p = V(floor.Location);
            s.ground[0] = p.x, s.ground[1] = p.y, s.ground[2] = p.z;
            s.radius = ab->TraceSphereRadiusOverride > 0 ? ab->TraceSphereRadiusOverride : 150.0f;
            Publish(s);
            return;
        }

        const aim::V3 end = aim::SweepEnd(cam, dir, start, range, viewHit, hitAt);
        EObjectTypeQuery typesArr[2] = {EObjectTypeQuery::ObjectTypeQuery3, EObjectTypeQuery::ObjectTypeQuery6};  // Pawn, Destructible
        const TArray<EObjectTypeQuery> types(typesArr, 2, 2);
        FHitResult hit{};
        const bool swept = UKismetSystemLibrary::SphereTraceSingleForObjects(hero, F(start), F(end), radius, types, false, ignored,
                                                                             EDrawDebugTrace::None, &hit, true, FLinearColor{}, FLinearColor{}, 0.0f);
        AActor* target = swept ? hit.Actor.Get() : nullptr;
        bool enemies = false;
        if (PtrOk(target)) {
            UWorld* w = UWorld::GetWorld();
            AGameStateBase* gs = PtrOk(w) ? w->GameState : nullptr;
            UFunction* fn = g_fnEnemies.Get();
            if (PtrOk(gs) && fn && gs->IsA(ABP_GameState_C::StaticClass())) {
                Params::BP_GameState_C_I_AreActorsEnemies p{};
                p.ActorA = target;
                p.ActorB = hero;
                gs->ProcessEvent(fn, &p);
                enemies = p.AreEnemies || p.CanDamage;
            } else {
                enemies = !(target->IsA(APawn::StaticClass()) && static_cast<APawn*>(target)->IsPlayerControlled());
            }
        }
        const bool character = PtrOk(target) && target->IsA(ACharacter::StaticClass());
        const aim::Kind mark = aim::Mark(int(ab->mDefaultAbilityTargetingType), character, target == hero, enemies, true);
        if (mark != aim::Kind::None) {
            auto* c = static_cast<ACharacter*>(target);
            const aim::V3 at = V(c->K2_GetActorLocation());
            const float half = PtrOk(c->CapsuleComponent) ? c->CapsuleComponent->GetScaledCapsuleHalfHeight() : 90.0f;
            s.hasTarget = true;
            s.head[0] = s.feet[0] = at.x, s.head[1] = s.feet[1] = at.y;
            s.head[2] = at.z + half, s.feet[2] = at.z - half;
            s.radius = PtrOk(c->CapsuleComponent) ? c->CapsuleComponent->GetScaledCapsuleRadius() * 1.4f : 50.0f;
            if (!g_lastTarget.Is(target)) { g_lastTarget = ref::Ref(target); s.serial = 1; }
        } else {
            g_lastTarget = {};
        }
        Publish(s);
    }

    cast_mode::Key KeyOf(const FKey& k) {
        if (!g_keysMade) {
            g_left = UKismetStringLibrary::Conv_StringToName(FString(L"LeftMouseButton"));
            g_right = UKismetStringLibrary::Conv_StringToName(FString(L"RightMouseButton"));
            g_keysMade = true;
        }
        return k.KeyName == g_left ? cast_mode::Key::Left : k.KeyName == g_right ? cast_mode::Key::Right : cast_mode::Key::Other;
    }

    // Before the game runs a key event: true = the game never sees it.
    bool Filter(void* objp, void* fnp, void* parms) {
        if (t_busy || !g_on.load(std::memory_order_relaxed) || !game::OnGameThread()) return false;
        const int i = InputIndex(fnp);
        if (i < 0 || !parms || !PtrOk(objp)) return false;
        auto* obj = static_cast<UObject*>(objp);
        if (obj != umg::LocalPC() || !obj->IsA(ABP_PlayerControllerGame_C::StaticClass())) return false;
        auto* pc = static_cast<ABP_PlayerControllerGame_C*>(obj);
        const FKey& key = *static_cast<FKey*>(parms);

        t_busy = true;
        cast_mode::Event e{};
        e.press = pc->IsInputKeyDown(key);
        e.key = KeyOf(key);
        e.slot = g_input[i].n > 0 ? cast_mode::SlotByte(g_input[i].n, pc->AbilityModifierPressed) : -1;
        Slot found;
        if (e.press && e.slot >= 0 && e.key == cast_mode::Key::Other && !(g_state.aiming && e.slot == g_state.slot)) {
            found = Lookup(pc, e.slot);
            e.aimed = found.aimed;
        }
        const int aimedSlot = g_state.slot;
        bool swallow = true;
        switch (cast_mode::Decide(g_state, e)) {
            case cast_mode::Do::Enter:
                g_aim = found;
                g_nextPredict = 0;
                logger::log(("[cast-mode] aiming " + found.name + (found.kind == aim::Kind::Ally ? " (ally)" : found.kind == aim::Kind::Enemy ? " (enemy)" : " (ground)")).c_str());
                Predict(pc);
                break;
            case cast_mode::Do::Confirm: Cast(pc, aimedSlot); break;
            case cast_mode::Do::Cancel: g_cancels++; End("cancelled"); break;
            case cast_mode::Do::CancelPass: g_cancels++; End("cancelled by another ability"); swallow = false; break;
            case cast_mode::Do::Swallow: break;
            case cast_mode::Do::Pass: swallow = false; break;
        }
        t_busy = false;
        return swallow;
    }

    // World tick: clicks that raise no ability event, and the marker follows the aim.
    void OnTick(void*, void*, void*) {
        if (t_busy || !g_on.load(std::memory_order_relaxed) || !game::OnGameThread()) return;
        APlayerController* pc0 = umg::LocalPC();
        if (!PtrOk(pc0) || !pc0->IsA(ABP_PlayerControllerGame_C::StaticClass())) { if (g_state.aiming) End("no controller"); return; }
        auto* pc = static_cast<ABP_PlayerControllerGame_C*>(pc0);
        if (!g_state.aiming && !g_state.leftHeld && !g_state.rightHeld) return;
        t_busy = true;
        FKey left{}, right{};
        KeyOf(left);  // makes the names
        left.KeyName = g_left, right.KeyName = g_right;
        if (g_state.leftHeld && !pc->IsInputKeyDown(left)) cast_mode::Released(g_state, cast_mode::Key::Left);
        if (g_state.rightHeld && !pc->IsInputKeyDown(right)) cast_mode::Released(g_state, cast_mode::Key::Right);
        if (g_state.aiming) {
            const int slot = g_state.slot;
            if (pc->WasInputKeyJustPressed(left) && cast_mode::Click(g_state, cast_mode::Key::Left) == cast_mode::Do::Confirm) Cast(pc, slot);
            else if (pc->WasInputKeyJustPressed(right) && cast_mode::Click(g_state, cast_mode::Key::Right) == cast_mode::Do::Cancel) { g_cancels++; End("cancelled"); }
        }
        if (g_state.aiming) {
            const ULONGLONG now = GetTickCount64();
            if (now >= g_nextPredict) { g_nextPredict = now + 33; Predict(pc); }
        }
        t_busy = false;
    }

    // ---- drawing (render thread) ----
    namespace spec {  // component geometry at 1080p (proposed in #104; design lead's spec pending)
        constexpr float kChevronW = 22, kChevronH = 16, kChevronGap = 14, kGlyph = 9, kRingSeg = 40, kStroke = 2.5f;
        constexpr float kFillAlpha = 0.14f, kNoTargetR = 16;
    }

    void Ring(ImDrawList* dl, const combat::View& v, const feature::Frame& f, const float c[3], float radius, style::Rgba col, float px, bool fill) {
        ImVec2 pts[int(spec::kRingSeg)];
        int n = 0;
        for (int i = 0; i < int(spec::kRingSeg); i++) {
            const float a = 6.2831853f * float(i) / spec::kRingSeg;
            float sx, sy;
            if (!combat::Project(v, c[0] + std::cos(a) * radius, c[1] + std::sin(a) * radius, c[2] + 2.0f, f.w, f.h, sx, sy)) return;
            pts[n++] = {sx, sy};
        }
        if (fill) dl->AddConvexPolyFilled(pts, n, style::Pack(col, spec::kFillAlpha));
        dl->AddPolyline(pts, n, style::Pack(style::color::kInk, style::stroke::kOutlineAlpha), ImDrawFlags_Closed, px + 2.0f);
        dl->AddPolyline(pts, n, style::Pack(col, 1.0f), ImDrawFlags_Closed, px);
    }

    struct CastMode : feature::Feature {
        bool listening = false;
        unsigned seenSerial = 0;
        double popAt = -10;

        CastMode() : Feature("Cast mode", feature::Stage::Alpha) {}

        void OnFrame(const feature::Frame& f) override {
            if (!listening) {
                listening = true;
                g_on = true;
                game::SetEventFilter(&Filter, true);
                game::OnWorldTick(&OnTick, true);
            }
            AcquireSRWLockShared(&g_mu);
            const Shown s = g_shown;
            ReleaseSRWLockShared(&g_mu);
            combat::View view{};
            if (!s.aiming || !game::GetView(view)) return;
            if (s.serial != seenSerial) { seenSerial = s.serial; popAt = f.now; }
            ImDrawList* dl = ImGui::GetBackgroundDrawList();
            const float ui = style::type::Ui(f.h);
            const style::Rgba col = s.want == aim::Kind::Ally ? style::color::kHeal : s.want == aim::Kind::Enemy ? style::color::kTargetEnemy : style::color::kAim;
            const ImU32 ink = style::Pack(style::color::kInk, style::stroke::kOutlineAlpha);

            if (s.want == aim::Kind::None) {
                Ring(dl, view, f, s.ground, s.radius, col, spec::kStroke * ui, true);
            } else if (!s.hasTarget) {  // aiming, nobody valid under the aim: a quiet ring at the crosshair
                dl->AddCircle({f.w * 0.5f, f.h * 0.5f}, spec::kNoTargetR * ui, ink, 32, spec::kStroke * ui + 2.0f);
                dl->AddCircle({f.w * 0.5f, f.h * 0.5f}, spec::kNoTargetR * ui, style::Pack(style::color::kTextMuted, 0.9f), 32, spec::kStroke * ui);
            } else {
                Ring(dl, view, f, s.feet, s.radius, col, spec::kStroke * ui, false);
                float sx, sy;
                if (combat::Project(view, s.head[0], s.head[1], s.head[2], f.w, f.h, sx, sy)) {
                    const float dx = s.head[0] - view.x, dy = s.head[1] - view.y, dz = s.head[2] - view.z;
                    const float depth = std::clamp(1500.0f / std::max(std::sqrt(dx * dx + dy * dy + dz * dz), 1.0f), 0.6f, 1.25f);
                    // pop when the target changes: starts small, overshoots, settles (motion::kPop)
                    const float t = std::clamp(float(f.now - popAt) / style::motion::kPop.dur, 0.0f, 1.0f);
                    const float k = ui * depth * (0.6f + 0.4f * style::ease::Apply(style::motion::kPop.curve, t, style::motion::kPop.k));
                    const float w = spec::kChevronW * k, h = spec::kChevronH * k, tip = sy - spec::kChevronGap * k;
                    const ImVec2 a{sx - w * 0.5f, tip - h}, b{sx + w * 0.5f, tip - h}, c{sx, tip};
                    dl->AddTriangleFilled(a, b, c, style::Pack(col, style::stroke::kGlowAlpha));
                    dl->AddTriangle(a, b, c, ink, spec::kStroke * k + 2.0f);
                    dl->AddTriangleFilled(a, b, c, style::Pack(col, 1.0f));
                    // glyph above the chevron: plus = ally, crosshair ring = enemy (colour is never the only channel)
                    const ImVec2 g{sx, tip - h - spec::kGlyph * k * 1.6f};
                    const float r = spec::kGlyph * k, th = std::max(2.0f, r * 0.42f);
                    if (s.want == aim::Kind::Ally) {
                        for (float grow : {2.0f, 0.0f}) {
                            const ImU32 cc = grow > 0 ? ink : style::Pack(col, 1.0f);
                            dl->AddRectFilled({g.x - r - grow, g.y - th * 0.5f - grow}, {g.x + r + grow, g.y + th * 0.5f + grow}, cc);
                            dl->AddRectFilled({g.x - th * 0.5f - grow, g.y - r - grow}, {g.x + th * 0.5f + grow, g.y + r + grow}, cc);
                        }
                    } else {
                        dl->AddCircle(g, r, ink, 24, th + 2.0f);
                        dl->AddCircle(g, r, style::Pack(col, 1.0f), 24, th);
                        dl->AddCircleFilled(g, th * 0.6f, style::Pack(col, 1.0f));
                    }
                }
            }
            const float ts = style::type::kSm * ui;
            const char* hint = "Left click: cast    Right click: cancel";
            const ImVec2 sz = f.font->CalcTextSizeA(ts, FLT_MAX, 0, hint);
            draw::OutlinedText(dl, f.font, ts, {f.w * 0.5f - sz.x * 0.5f, f.h * 0.62f}, style::Pack(style::color::kTextSoft, 1.0f), ink,
                               style::stroke::Outline(ts), hint);
        }

        void Off() override {
            g_on = false;
            if (listening) {
                game::SetEventFilter(&Filter, false);
                game::OnWorldTick(&OnTick, false);
            }
            listening = false;
            AcquireSRWLockExclusive(&g_mu);
            g_shown = {};
            ReleaseSRWLockExclusive(&g_mu);
            g_state = {};  // listeners are gone: no game-thread reader left
        }

        void Menu() override {
            bool a = g_allies, e = g_enemies;
            if (ImGui::Checkbox("Ally abilities aim first (heals, buffs)", &a)) { g_allies = a; ImGui::MarkIniSettingsDirty(); }
            if (ImGui::Checkbox("Enemy-target abilities aim first", &e)) { g_enemies = e; ImGui::MarkIniSettingsDirty(); }
            ImGui::TextDisabled("Also aim first, with a ground circle (area abilities), slot:");
            int mask = g_groundMask;
            for (int i = 0; i < 12; i++) {
                bool on = mask >> i & 1;
                char id[8];
                std::snprintf(id, sizeof(id), "%d", i + 1);
                if (i % 6) ImGui::SameLine();
                if (ImGui::Checkbox(id, &on)) { mask = on ? mask | 1 << i : mask & ~(1 << i); g_groundMask = mask; ImGui::MarkIniSettingsDirty(); }
            }
            ImGui::TextDisabled("press: aim   left click / same key: cast   right click: cancel   (%d cast, %d cancelled)", g_casts.load(), g_cancels.load());
        }

        void Load(const char* key, const char* value) override {
            const std::string k = key;
            if (k == "allies") g_allies = std::atoi(value) != 0;
            else if (k == "enemies") g_enemies = std::atoi(value) != 0;
            else if (k == "ground") g_groundMask = std::atoi(value);
        }
        void Save(std::vector<std::pair<std::string, std::string>>& out) override {
            out.push_back({"allies", g_allies ? "1" : "0"});
            out.push_back({"enemies", g_enemies ? "1" : "0"});
            out.push_back({"ground", std::to_string(g_groundMask.load())});
        }
    } g_castMode;
}

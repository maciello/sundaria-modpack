#include "feature.hpp"
#include "charge-bar.hpp"
#include "logger.hpp"
#include "ref.hpp"
#include "style.hpp"
#include "umg.hpp"
#include "imgui.h"

#include <Windows.h>
#include <atomic>
#include <string>
#include "Engine_classes.hpp"
#include "Archon_classes.hpp"
#include "GameplayAbilities_classes.hpp"
#include "BP_GameAbilityBase_classes.hpp"

// Charge bar (#82): while the hero's animating ability counts down its cast time or climbs hold levels, a bar under
// the character fills; full = armed. Game thread (ProcessEvent listener, ~60 Hz, O(1)) reads the ability's fields and
// publishes plain numbers; the render thread only animates them.
using namespace SDK;

namespace {
    using umg::PtrOk;
    using namespace charge_bar;

    std::atomic<bool> g_on{false};

    // Game thread state.
    ref::Ref g_lastAbility;
    Arm g_arm;
    double g_lastTick = 0;

    SRWLOCK g_mu = SRWLOCK_INIT;
    Arm g_pub;  // game thread → render thread

    void Track() {
        APlayerController* pc = umg::LocalPC();
        if (!pc || !PtrOk(pc->Pawn) || !pc->Pawn->IsA(AArchonCharacter::StaticClass())) return;
        UAbilitySystemComponent* asc = static_cast<AArchonCharacter*>(pc->Pawn)->mAbilitySystemComponent;
        if (!PtrOk(asc)) return;
        // ponytail: only the animating ability; a cast time without a cast animation shows nothing (scan
        // ActivatableAbilities if the log shows such abilities)
        UGameplayAbility* ab = asc->LocalAnimMontageInfo.AnimatingAbility;
        if (!PtrOk(ab) || !ab->IsA(UArchonGameplayAbility::StaticClass())) ab = nullptr;
        if (!(ab ? g_lastAbility.Is(ab) : !g_lastAbility.ptr)) {
            g_lastAbility = ref::Ref(ab);
            g_arm = {g_arm.id + 1, false, 0, 0, 0, 0, ab ? ab->Class->GetName() : std::string()};
        }
        g_arm.active = ab != nullptr;
        if (ab) {
            const auto* a = static_cast<const UArchonGameplayAbility*>(ab);
            g_arm.remain = a->mStartedCast ? a->mLocalCastTimeRemaining : 0;
            if (ab->IsA(UBP_GameAbilityBase_C::StaticClass())) {
                const auto* b = static_cast<const UBP_GameAbilityBase_C*>(ab);
                g_arm.level = b->mHoldLevel;
                g_arm.maxLevel = b->kMaxHoldLevel;
                g_arm.interval = b->mHoldInterval;
            }
        }
        AcquireSRWLockExclusive(&g_mu);
        g_pub = g_arm;
        ReleaseSRWLockExclusive(&g_mu);
    }

    void OnEvent(void*, void*, void*) {
        if (!g_on.load(std::memory_order_relaxed) || !game::OnGameThread()) return;
        const double now = GetTickCount64() / 1000.0;
        if (now - g_lastTick < 0.015) return;
        g_lastTick = now;
        Track();
    }

    struct ChargeBar : feature::Feature {
        Bar bar;
        std::string last;

        ChargeBar() : Feature("Charge bar", feature::Stage::Alpha) { optIn = true; }  // new game-thread hook

        void OnFrame(const feature::Frame& f) override {
            if (!g_on) { g_on = true; game::SetEventListener(&OnEvent, true); }
            AcquireSRWLockShared(&g_mu);
            const Arm a = g_pub;
            ReleaseSRWLockShared(&g_mu);
            if (bar.Update(a, f.now)) { last = bar.log; logger::log("[charge-bar] " + last); }
            if (bar.Visible(f.now)) Draw(ImGui::GetForegroundDrawList(), f);  // Layer::Hud
        }

        void Draw(ImDrawList* dl, const feature::Frame& f) const {
            using namespace style;
            const float ui = type::Ui(f.h), alpha = bar.Alpha(f.now), s = bar.Scale(f.now);
            const float w = hud::kBarW * ui * s, h = hud::kBarH * ui * s;
            const float x0 = f.w * 0.5f - w * 0.5f, y0 = f.h * hud::kChargeY - h * 0.5f;
            dl->AddRectFilled({x0, y0}, {x0 + w, y0 + h}, Pack(color::kTrack, alpha), radius::Pill(h));
            const Rgba fill = bar.cancelled ? color::kTextMuted : Mix(color::kTextSoft, color::kText, bar.Flash(f.now));
            if (bar.progress > 0) dl->AddRectFilled({x0, y0}, {x0 + w * bar.progress, y0 + h}, Pack(fill, alpha), radius::Pill(h));
            for (int i = 1; i < bar.segs; i++)
                dl->AddLine({x0 + w * i / bar.segs, y0}, {x0 + w * i / bar.segs, y0 + h}, Pack(color::kInk, alpha), 1);
            dl->AddRect({x0, y0}, {x0 + w, y0 + h}, Pack(color::kTextSoft, .6f * alpha), radius::Pill(h), 0, 1);
        }

        // The listener has drained when SetEventListener returns: the game-thread state is ours again.
        void Off() override {
            g_on = false;
            game::SetEventListener(&OnEvent, false);
            g_lastAbility = {};
            g_arm = g_pub = {};
            bar = {};
        }

        void Menu() override {
            if (!last.empty()) ImGui::TextDisabled("last: %s", last.c_str());
        }
    } g_charge_bar;
}

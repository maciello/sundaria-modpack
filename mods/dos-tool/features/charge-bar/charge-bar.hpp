#pragma once
#include <algorithm>
#include <cstdio>
#include <string>
#include "style.hpp"

// Charge bar (#82): a bar under the character that fills while the hero's ability arms: its cast time
// (mLocalCastTimeRemaining) or its hold levels (mHoldLevel of kMaxHoldLevel, one per mHoldInterval).
// SDK-free: the game thread publishes Arm, the render thread animates Bar. Spec: design-system.md § Charge bar.
namespace charge_bar {
    constexpr float kPunch = 1.1f, kPunchDur = 0.25f, kHold = 0.3f;  // ready: punch, then hold before the fade (cast)

    // Game thread → render thread. id changes per animating ability; active = it still animates.
    struct Arm { unsigned id = 0; bool active = false; float remain = 0; int level = 0, maxLevel = 0; float interval = 0; std::string ability; };

    struct Bar {
        unsigned id = 0;
        bool show = false, cancelled = false;
        double shown = -1e9, readyAt = -1, endAt = -1, levelAt = 0;
        float total = 0, progress = 0, interval = 0;
        int segs = 0, level = -1;
        std::string ability, log;  // log: summary of the arm that just ended

        // true once when a shown arm ends (caller logs `log`).
        bool Update(const Arm& a, double now) {
            bool ended = false;
            if (a.id != id) {
                ended = End(now);
                const std::string keep = log;
                *this = {};
                log = keep;
                id = a.id;
                ability = a.ability;
            }
            if (!a.active) return End(now) || ended;
            if (a.remain > 0) {  // cast time
                if (!show) { show = true; shown = now; }
                total = std::max(total, a.remain);
                progress = 1 - a.remain / total;
            } else if (a.maxLevel > 0 && a.level >= 1) {  // holding: level 0 is indistinguishable from a plain shot
                if (!show) { show = true; shown = now; }
                segs = a.maxLevel;
                if (a.level != level) { level = a.level; levelAt = now; }
                interval = a.interval;
                const float frac = level >= segs || interval <= 0 ? 0 : std::clamp(float((now - levelAt) / interval), 0.0f, 1.0f);
                progress = std::min(1.0f, (level + frac) / segs);
            } else if (show && total > 0) {
                progress = 1;  // cast time ran out
            }
            if (show && progress >= 0.999f && readyAt < 0) readyAt = now;
            return ended;
        }

        bool End(double now) {
            if (!show || endAt >= 0) return false;
            endAt = now;
            cancelled = readyAt < 0;
            char b[96];
            std::snprintf(b, sizeof b, " cast %.2fs hold %d/%d%s", total, std::max(level, 0), segs, cancelled ? " cancelled" : "");
            log = ability + b;
            return true;
        }

        double FadeStart() const {
            if (endAt >= 0) return segs == 0 && readyAt >= 0 ? std::min(endAt, readyAt + kPunchDur + kHold) : endAt;
            return segs == 0 && readyAt >= 0 ? readyAt + kPunchDur + kHold : 1e18;  // a held bar stays full until release
        }
        bool Visible(double now) const { return show && now < FadeStart() + style::motion::kFadeOut.dur; }
        float Alpha(double now) const {
            using namespace style::motion;
            const float in = style::ease::Apply(kFadeIn.curve, float((now - shown) / kFadeIn.dur));
            return in * (1 - style::ease::Apply(kFadeOut.curve, float((now - FadeStart()) / kFadeOut.dur)));
        }
        float Scale(double now) const {
            if (readyAt < 0) return 1;
            return kPunch - (kPunch - 1) * style::ease::Apply(style::ease::Curve::OutCubic, float((now - readyAt) / kPunchDur));
        }
        float Flash(double now) const {
            return readyAt < 0 ? 0 : 1 - std::clamp(float((now - readyAt) / style::motion::kFlash.dur), 0.0f, 1.0f);
        }
    };
}

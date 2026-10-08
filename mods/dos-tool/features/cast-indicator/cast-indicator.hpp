#pragma once
#include <algorithm>
#include "style.hpp"

// Cast indicator (#3): one pip per hit the playing montage should land, filled per landed hit.
// SDK-free: the game thread publishes Cast, the render thread animates Pips. Spec: design-system.md § Hit pips.
namespace cast_indicator {
    constexpr int kMaxHits = 64;
    constexpr int kMinHits = 2;         // single-hit casts get no indicator
    constexpr int kBarFrom = 11;        // N > 10: one segmented bar
    constexpr float kPipHalf = 6;       // diamond half-diagonal, × Ui
    constexpr float kBarW = 120, kBarH = 6;  // × Ui
    constexpr float kAnchorY = 0.62f;   // row centre, × screen height
    constexpr float kGlowR = 1.8f;      // glow disc radius, × kPipHalf
    constexpr float kPunch = 1.15f, kPunchDur = 0.25f, kHold = 0.5f;
    constexpr double kLateHits = 0.6;   // s after the montage ends that arrows in flight still count (tuning knob)

    // UBP_GameplayAnimNotify_C::mGameplayAnimNotifyType: ApplyEffect 0, ShootProjectile 1 are hits.
    inline bool IsHit(int notifyType) { return notifyType == 0 || notifyType == 1; }

    // Game thread → render thread. cast changes per montage start; done = no more hits will count.
    struct Cast { unsigned cast = 0; int hits = 0, landed = 0; bool done = true; };

    struct Pips {
        unsigned seen = 0;              // last Cast::cast looked at (shown or not)
        int hits = 0, landed = 0;
        double shown = -1e9, fullAt = -1, doneAt = -1;
        double fillAt[kMaxHits] = {};

        void Update(const Cast& c, double now) {
            if (c.hits < kMinHits) {  // a cast without an indicator ends the shown one
                if (c.cast != seen && doneAt < 0) doneAt = now;
                seen = c.cast;
                return;
            }
            if (c.cast != seen) { seen = c.cast; hits = std::min(c.hits, kMaxHits); landed = 0; shown = now; fullAt = doneAt = -1; }
            while (landed < std::min(c.landed, hits)) fillAt[landed++] = now;
            if (landed == hits && fullAt < 0) fullAt = now;
            if (c.done && doneAt < 0) doneAt = now;
        }

        double FadeStart() const {
            if (fullAt >= 0) return fullAt + kPunchDur + kHold;
            return doneAt >= 0 ? doneAt : 1e18;
        }
        bool Visible(double now) const { return hits > 0 && now < FadeStart() + style::motion::kFadeOut.dur; }

        float Alpha(double now) const {
            using namespace style::motion;
            const float in = style::ease::Apply(kFadeIn.curve, float((now - shown) / kFadeIn.dur));
            const float out = style::ease::Apply(kFadeOut.curve, float((now - FadeStart()) / kFadeOut.dur));
            return in * (1 - out);
        }
        float RowScale(double now) const {
            if (fullAt < 0) return 1;
            return kPunch - (kPunch - 1) * style::ease::Apply(style::ease::Curve::OutCubic, float((now - fullAt) / kPunchDur));
        }
        bool Filled(int i) const { return i < landed; }
        bool Muted(int i) const { return doneAt >= 0 && i >= landed; }  // cast ended without this hit
        float PipScale(int i, double now) const {
            if (!Filled(i)) return 1;
            const auto& m = style::motion::kPipFill;
            const float t = float((now - fillAt[i]) / m.dur);
            return 0.4f + 0.6f * style::ease::Apply(m.curve, t, m.k);
        }
        float Flash(int i, double now) const {  // white flash 1 → 0 after the fill
            if (!Filled(i)) return 0;
            return 1 - std::clamp(float((now - fillAt[i]) / style::motion::kFlash.dur), 0.0f, 1.0f);
        }
    };

    // Pip i's centre x in a row of n centred at cx (ui = Ui(h)).
    inline float PipX(int i, int n, float cx, float ui) {
        const float step = (2 * kPipHalf + style::space::k2) * ui;
        return cx + (i - (n - 1) * 0.5f) * step;
    }

    // Game-thread bookkeeping of one cast: montage ended → late hits for kLateHits, then done.
    struct Tracker {
        Cast c;
        double endedAt = -1;

        void Begin(int hits) { c = {c.cast + 1, hits, 0, false}; endedAt = -1; }
        void Hit() { if (!c.done && c.landed < c.hits) c.landed++; }
        void End(double now) { if (!c.done && endedAt < 0) endedAt = now; }
        // true once when the cast becomes done (caller logs it).
        bool Tick(double now) {
            if (c.done) return false;
            if (c.landed >= c.hits || (endedAt >= 0 && now - endedAt >= kLateHits)) { c.done = true; return true; }
            return false;
        }
        bool Finish() { if (c.done) return false; c.done = true; return true; }  // a new cast cuts this one
    };
}

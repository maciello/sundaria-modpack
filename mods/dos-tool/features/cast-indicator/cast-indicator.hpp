#pragma once
#include <algorithm>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>
#include "combat.hpp"
#include "timeline.hpp"
#include "style.hpp"

// Cast indicator (#3): one pip per hit the playing montage should land, filled per landed hit.
// SDK-free: the game thread publishes Montage, the render thread counts landed hits (Tracker + Records) and animates Pips. Spec: design-system.md § Hit pips.
namespace cast_indicator {
    constexpr int kMaxHits = 64;
    constexpr int kMinHits = 2;         // single-hit casts get pips only after a wind-up (PipsFor)
    constexpr int kBarFrom = 11;        // N > 10: one segmented bar
    constexpr float kPipHalf = 6;       // diamond half-diagonal, × Ui
    constexpr float kGlowR = 1.8f;      // glow disc radius, × kPipHalf
    constexpr float kPunch = 1.15f, kPunchDur = 0.25f, kHold = 0.5f;
    constexpr double kLateHits = 0.6;   // s after the montage ends that arrows in flight still count (tuning knob)
    constexpr float kWindupMin = 0.3f;  // s before the first hit from which a cast gets the wind-up ring (tuning knob)
    constexpr float kRingR = 11, kApproachR = 44;  // target ring / approach ring start radius, × Ui
    constexpr float kRelease = 0.25f, kReleasePunch = 1.35f;  // release: target ring punches out and fades

    // UBP_GameplayAnimNotify_C::mGameplayAnimNotifyType: ApplyEffect 0, ShootProjectile 1 are hits.
    inline bool IsHit(int notifyType) { return notifyType == 0 || notifyType == 1; }

    // Pips a cast shows: multi-hit casts, or one pip for a single hit after a wind-up (the ring lands on it).
    // Spread abilities show none: their notify count is not hits per target (#81: Salvo "hits 20, landed 4-9").
    inline int PipsFor(int hits, float windup, bool spread) {
        if (spread) return 0;
        return hits >= kMinHits || (hits == 1 && windup >= kWindupMin) ? hits : 0;
    }
    // ponytail: one known cone ability by class name; volley count from ApplyEffectID once the notify dump shows its grouping
    inline bool Spread(const std::string& ability) { return ability == "BP_GameAbility_Salvo_C"; }

    // Game thread → render thread: the hero's current montage that shows something. cast changes per montage start.
    // hits = pips (0: ring only); windup = s from start to the first hit; fireAt = steady-clock s of that hit (live).
    struct Montage {
        unsigned cast = 0; int hits = 0; bool ended = true; std::uintptr_t hero = 0; std::string ability, name;
        float windup = 0; double fireAt = 0;
    };
    // Tracker → Pips. done = no more hits will count.
    struct Cast { unsigned cast = 0; int hits = 0, landed = 0; bool done = true; };

    // Landed hits = new hit records (AArchonCharacter::LastTakeHitInfo, sampled by core) on non-players whose
    // instigator is the hero. Keeps every character's last stamp so only records made after a sample count.
    struct Records {
        std::unordered_map<std::uintptr_t, unsigned> stamp;
        // Returns the hero's new records; every new record (anyone's) goes to `fresh` (trace).
        int New(const std::vector<combat::Sample>& chars, std::uintptr_t hero, std::vector<const combat::Sample*>* fresh = nullptr) {
            std::unordered_map<std::uintptr_t, unsigned> next;
            int n = 0;
            for (const combat::Sample& s : chars) {
                next[s.id] = s.hitStamp;
                auto it = stamp.find(s.id);
                if (it == stamp.end() || it->second == s.hitStamp) continue;
                if (fresh) fresh->push_back(&s);
                if (!s.isPlayer && hero && s.hitBy == hero) n++;
            }
            stamp.swap(next);  // characters that left are forgotten
            return n;
        }
    };

    struct Pips {
        unsigned seen = 0;              // last Cast::cast looked at (shown or not)
        int hits = 0, landed = 0;
        double shown = -1e9, fullAt = -1, doneAt = -1;
        double fillAt[kMaxHits] = {};

        void Update(const Cast& c, double now) {
            if (c.hits < 1) {  // a cast without pips ends the shown ones
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

    // Wind-up ring (render thread): an approach ring shrinks linearly from kApproachR onto the target ring and meets it at
    // fireAt; release = punch + flash + fade; montage ended first = cancelled (muted, fade).
    struct Ring {
        unsigned cast = 0;
        float total = 0;
        double shown = -1e9, fireAt = 1e18, endAt = -1;

        // fireAt: this frame's estimate (render clock); frozen once reached.
        void Update(unsigned c, float windup, double fire, bool ended, double now) {
            if (c != cast) { *this = {}; cast = c; total = windup; shown = now; }
            if (total < kWindupMin) return;
            if (!Fired(now) && endAt < 0) fireAt = fire;
            if (ended && endAt < 0) endAt = now;
        }
        bool Fired(double now) const { return now >= fireAt && !Cancelled(); }
        bool Cancelled() const { return endAt >= 0 && endAt < fireAt; }
        double Until() const { return Cancelled() ? endAt + style::motion::kFadeOut.dur : fireAt + kRelease; }
        bool Visible(double now) const { return total >= kWindupMin && now < Until(); }
        float Approach(double now) const {  // approach radius, × Ui
            return kRingR + (kApproachR - kRingR) * std::clamp(float((fireAt - now) / total), 0.0f, 1.0f);
        }
        float Alpha(double now) const {
            using namespace style::motion;
            const float in = style::ease::Apply(kFadeIn.curve, float((now - shown) / kFadeIn.dur));
            if (Cancelled()) return in * (1 - style::ease::Apply(kFadeOut.curve, float((now - endAt) / kFadeOut.dur)));
            if (!Fired(now)) return in;
            return 1 - std::clamp(float((now - fireAt) / kRelease), 0.0f, 1.0f);
        }
        float Punch(double now) const {  // target ring scale
            if (!Fired(now)) return 1;
            return 1 + (kReleasePunch - 1) * style::ease::Apply(style::ease::Curve::OutCubic, float((now - fireAt) / kRelease));
        }
        float Flash(double now) const {
            return Fired(now) ? 1 - std::clamp(float((now - fireAt) / style::motion::kFlash.dur), 0.0f, 1.0f) : 0;
        }
    };

    // One cast's bookkeeping (render thread): montage ended → late hits for kLateHits, then done.
    struct Tracker {
        Cast c;
        double endedAt = -1;

        void Begin(unsigned cast, int hits) { c = {cast, hits, 0, false}; endedAt = -1; }
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

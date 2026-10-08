#pragma once
// SDK-free logic for Input feel: what to do with an ability press that lands while the
// current ability holds the animation lock (the game would queue the press until the lock ends).
namespace input_feel {
    // Windup = the ability's effect has not fired yet (no ApplyEffect/ShootProjectile notify).
    // Out = it fired; what's left is recovery animation.
    enum class Phase { Idle, Windup, Out };

    struct Options {
        bool cancelWindup = true;    // a different ability pressed in windup replaces it (Overwatch reload cancel)
        bool cancelRecovery = true;  // a press after the ability is out skips its recovery animation
        bool refundCooldown = true;  // a windup cancel gives the cancelled ability's cooldown back
    };

    struct Action {
        bool cancelCurrent = false;  // cancel the current ability (its effect never happens)
        bool removeLock = false;     // end the animation lock, so the game fires the queued press now
        bool refundCooldown = false; // remove the cooldown the cancelled ability already started
    };

    // One montage play of the animating ability (ASC LocalAnimMontageInfo). PlayBit flips per play, so a recast of the
    // same ability instance (instanced per actor) is a new play and starts in windup again.
    struct Play {
        const void* ability = nullptr;
        const void* montage = nullptr;
        bool bit = false;
        bool operator==(const Play&) const = default;
    };

    // out = the play whose effect notify fired last.
    inline Phase PhaseOf(const Play& now, const Play& out) {
        if (!now.ability) return Phase::Idle;
        return now == out ? Phase::Out : Phase::Windup;
    }

    // sameAbility: the press is for the ability that is currently animating (mashing, combos):
    // never cancel it, the game's own queue/combo handles that.
    inline Action Decide(Phase phase, bool locked, bool sameAbility, const Options& o) {
        Action a;
        if (!locked || sameAbility || phase == Phase::Idle) return a;
        if (phase == Phase::Windup && o.cancelWindup) {
            a.cancelCurrent = true; a.removeLock = true; a.refundCooldown = o.refundCooldown;
        }
        if (phase == Phase::Out && o.cancelRecovery) a.removeLock = true;
        return a;
    }
}

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

    // At most one cancel/cut per real key press. The game's queue retries a blocked press every tick and keeps
    // copies of it (lastFailureAbilityInputID + SharedInputs), and a refunded ability passes its cooldown check
    // again, so without this, queued presses cancel each other in a cycle (#28: ~900 cancels in one burst).
    struct Gate {
        unsigned presses = 0, spent = 0;
        void Press() { presses++; }
        bool MayAct() const { return presses != spent; }  // a real press arrived since our last action
        void Acted() { spent = presses; }
    };

    // When to act (#112): never inside the failure callback. K2_OnAbilityFailed fires inside FlushInputs, which goes
    // on to run the rest of the queue in the same call (hold-to-repeat inputs last): a lock removed there lets the
    // held ability (AimedShot) restart at once and B is blocked again; cancel after cancel, nothing fires.
    // So: decide in the callback, act after FlushInputs (world tick), and only if the game's next FlushInputs
    // retries B first (lastFailureAbilityInputID == B's input id): then B, not the queue, takes the freed lock.
    inline bool ActNow(bool pending, bool sameAnimating, int lastFailureId, int pressedId, int noOpId) {
        return pending && sameAnimating && pressedId != noOpId && lastFailureId == pressedId;
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

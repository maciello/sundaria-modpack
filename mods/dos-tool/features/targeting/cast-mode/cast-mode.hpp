#pragma once

// Cast mode (#107) logic, SDK-free: one press of an aimed ability starts aiming instead of casting; left click (or
// the same key again) casts, right click cancels. Decides what happens to each of the game's ability key events.
namespace cast_mode {
    // InpActEvt_Ability<N> -> the game's EAbilityInputName byte (BP_PlayerControllerGame ubergraph: slots 1..6 enqueue
    // N-1, or N+5 while the ability-bar modifier is held; slots 7..12 enqueue N-1).
    inline int SlotByte(int n, bool modifier) { return n >= 1 && n <= 6 && modifier ? n + 5 : n - 1; }

    enum class Key { Other, Left, Right };  // the key that raised the event
    enum class Do {
        Pass,            // not ours: the game handles it
        Swallow,         // the game does not see it, nothing else happens
        Enter,           // swallow, start aiming with this slot
        Confirm,         // swallow, cast the aimed ability now
        Cancel,          // swallow, stop aiming
        CancelPass,      // stop aiming, the game handles the event (another ability was pressed)
    };

    struct Event {
        bool press;      // key down (else key up)
        int slot;        // EAbilityInputName byte, -1 = not an ability slot (block, ...)
        Key key;
        bool aimed;      // press only: the slot's ability takes part in cast mode and is off cooldown
    };

    struct State {
        bool aiming = false;
        int slot = -1;           // slot being aimed
        int heldSlot = -1;       // slot whose press we swallowed: its release is swallowed too
        bool leftHeld = false, rightHeld = false;  // a click we used: swallow the button's events until it is up
    };

    inline Do Decide(State& s, const Event& e) {
        // a mouse button we already used for confirm/cancel: the game sees nothing of that click
        if (e.key == Key::Left && s.leftHeld) { if (!e.press) s.leftHeld = false; return Do::Swallow; }
        if (e.key == Key::Right && s.rightHeld) { if (!e.press) s.rightHeld = false; return Do::Swallow; }
        if (!e.press) {
            if (e.slot >= 0 && e.slot == s.heldSlot) { s.heldSlot = -1; return Do::Swallow; }
            return Do::Pass;
        }
        bool dropped = false;
        if (s.aiming) {
            if (e.key == Key::Left) { s.aiming = false; s.leftHeld = true; return Do::Confirm; }
            if (e.key == Key::Right) { s.aiming = false; s.rightHeld = true; return Do::Cancel; }
            if (e.slot == s.slot) { s.aiming = false; s.heldSlot = e.slot; return Do::Confirm; }  // same key again
            if (e.slot < 0) return Do::Pass;  // e.g. block on a keyboard key: not our business
            s.aiming = false;                 // another ability: this one is dropped, then treated as a fresh press
            dropped = true;
        }
        // a mouse button never starts aiming (basic attack / block live there)
        if (e.aimed && e.slot >= 0 && e.key == Key::Other) {
            s.aiming = true;
            s.slot = e.slot;
            s.heldSlot = e.slot;
            return Do::Enter;
        }
        return dropped ? Do::CancelPass : Do::Pass;
    }

    // The click polled from the controller (a mouse button bound to no ability event never reaches Decide).
    inline Do Click(State& s, Key k) {
        if (!s.aiming) return Do::Pass;
        s.aiming = false;
        if (k == Key::Left) { s.leftHeld = true; return Do::Confirm; }
        s.rightHeld = true;
        return Do::Cancel;
    }
    // The polled button is up again: stop swallowing its events (covers buttons that raise no release event).
    inline void Released(State& s, Key k) { (k == Key::Left ? s.leftHeld : s.rightHeld) = false; }
}

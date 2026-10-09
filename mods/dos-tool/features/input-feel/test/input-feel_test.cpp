// just test
#include "input-feel.hpp"
#include <cassert>
#include <cstdio>
#include <initializer_list>

using namespace input_feel;

int main() {
    const Options on;

    // Different ability pressed before the current one is out: cancel it, start the new one.
    Action a = Decide(Phase::Windup, true, false, on);
    assert(a.cancelCurrent && a.removeLock && a.refundCooldown);

    // Current ability already out: keep its effect, skip only the recovery.
    a = Decide(Phase::Out, true, false, on);
    assert(!a.cancelCurrent && a.removeLock && !a.refundCooldown);  // it happened: cooldown stays

    // Same ability (mashing / combo chain): leave it to the game.
    a = Decide(Phase::Windup, true, true, on);
    assert(!a.cancelCurrent && !a.removeLock);
    a = Decide(Phase::Out, true, true, on);
    assert(!a.cancelCurrent && !a.removeLock);

    // Not locked or nothing animating: the game starts the press itself.
    a = Decide(Phase::Windup, false, false, on);
    assert(!a.cancelCurrent && !a.removeLock);
    a = Decide(Phase::Idle, true, false, on);
    assert(!a.cancelCurrent && !a.removeLock);

    // Each half can be switched off on its own.
    Options noWindup; noWindup.cancelWindup = false;
    a = Decide(Phase::Windup, true, false, noWindup);
    assert(!a.cancelCurrent && !a.removeLock);
    Options noRecovery; noRecovery.cancelRecovery = false;
    a = Decide(Phase::Out, true, false, noRecovery);
    assert(!a.cancelCurrent && !a.removeLock);

    Options noRefund; noRefund.refundCooldown = false;
    a = Decide(Phase::Windup, true, false, noRefund);
    assert(a.cancelCurrent && !a.refundCooldown);

    // Phase per montage play: a recast of the same instance (PlayBit flipped) is windup again.
    int ab, mon;
    const Play out{&ab, &mon, true};
    assert(PhaseOf(out, out) == Phase::Out);
    assert(PhaseOf({&ab, &mon, false}, out) == Phase::Windup);
    assert(PhaseOf({}, out) == Phase::Idle);

    // The #28 loop: A animating, the queue holds stale presses of B, C, D that keep failing on the lock.
    // Only real presses may act: one press, one action, however often the queue retries.
    {
        Gate g;
        assert(!g.MayAct());            // queue retry with no press: never act
        g.Press();                      // real press of B while A winds up
        assert(g.MayAct());
        g.Acted();
        int actions = 1;
        for (int tick = 0; tick < 1000; tick++)
            if (g.MayAct()) { g.Acted(); actions++; }
        assert(actions == 1);
        g.Press();
        assert(g.MayAct());
    }

    // #112: holding AimedShot (hold-to-repeat) and pressing B. A model of the game's FlushInputs (bytecode,
    // BP_PlayerControllerGame_C::FlushInputs): a failed press is retried first (lastFailureAbilityInputID, then
    // return); otherwise the queued inputs run in one call, hold-to-repeat ones last. Cancelling A inside B's failure
    // callback lets the held A restart in the same call, so B never starts (the drop). Acting after the flush works.
    {
        const int noOp = -1, A = 1, B = 2;
        struct Model {
            int anim = A, lastFailure = -1;
            bool lock = true, pending = false, deferred;
            int starts[3] = {};
            void Press(int id) {
                if (lock) {
                    if (lastFailure == -1 || lastFailure == id) lastFailure = id;  // the game's K2_OnAbilityFailed handler
                    if (id == B && anim == A) {                                    // input-feel decides on B's block
                        if (deferred) pending = true;
                        else { anim = 0; lock = false; }                           // old: cancel A right here
                    }
                    return;
                }
                anim = id; lock = true; starts[id]++;
            }
            void Flush(bool pressB) {  // one tick: ReceiveTick -> FlushInputs
                if (lastFailure != -1) { const int id = lastFailure; lastFailure = -1; Press(id); return; }
                if (pressB) Press(B);
                Press(A);              // A's key is held: hold-to-repeat input, sorted last
            }
            void AfterFlush() {        // input-feel's world tick
                if (ActNow(pending, anim == A, lastFailure, B, -1)) { anim = 0; lock = false; }
                pending = false;
            }
        };
        for (const bool deferred : {false, true}) {
            Model m; m.deferred = deferred;
            m.Flush(true); m.AfterFlush();
            m.Flush(false); m.AfterFlush();
            if (deferred) assert(m.starts[B] == 1 && m.anim == B);   // B starts on the next tick
            else assert(m.starts[B] == 0 && m.starts[A] == 1);       // the bug: A restarted, B dropped
        }
        assert(!ActNow(true, true, 7, B, noOp));     // the game will retry another press first: leave A alone
        assert(!ActNow(true, false, B, B, noOp));    // A already ended
        assert(!ActNow(true, true, B, noOp, noOp));  // B has no input slot
    }

    std::puts("ok");
}

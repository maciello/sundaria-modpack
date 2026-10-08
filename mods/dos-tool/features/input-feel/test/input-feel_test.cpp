// just test
#include "input-feel.hpp"
#include <cassert>
#include <cstdio>

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
    // Only real presses may act: one press, one cancel, however often the queue retries.
    {
        Gate g;
        int A, B, C;
        assert(!g.MayAct());            // queue retry with no press: never act
        g.Press();                      // real press of B while A winds up
        assert(g.MayAct());
        g.Acted(&A);                    // B's failure cancels A
        int actions = 1;
        for (int tick = 0; tick < 1000; tick++)  // queued C / ghost A retry every tick against B's lock
            if (g.MayAct()) { g.Acted(&B); actions++; }
        assert(actions == 1);
        assert(g.IsGhost(&A) && !g.IsGhost(&B) && !g.IsGhost(&C));  // A restarting from the queue is a ghost
        g.ghosts = 4;
        assert(!g.IsGhost(&A));         // bounded
        g.Press();                      // a new real press may act again, and A is no longer a ghost
        assert(g.MayAct() && !g.IsGhost(&A));
    }

    std::puts("ok");
}

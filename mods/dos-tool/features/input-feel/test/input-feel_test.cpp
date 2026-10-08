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

    std::puts("ok");
}

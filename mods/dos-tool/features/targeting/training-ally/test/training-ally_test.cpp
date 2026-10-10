// just test
#include "training-ally.hpp"
#include <cassert>
#include <cstdio>

using namespace training_ally;

int main() {
    assert(!Changed(100, 100) && !Changed(0, 0.001f) && !Changed(99999904.f, 99999912.f));  // float noise on huge health
    assert(Changed(0, 0.1f) && Changed(5, 15) && Changed(1, 0.8f));

    assert(Num(291) == "291" && Num(0.25f) == "0.25" && Num(15) == "15");
    assert(Num(12500) == "12.5k" && Num(99999904.f) == "100.0M");

    assert(Line(kRows[Crit], 5, 5) == "Crit chance 5%");
    assert(Line(kRows[Crit], 5, 15) == "Crit chance 5% -> 15% (+10)");
    assert(Line(kRows[Cooldown], 1, 0.8f) == "Cooldowns x 1 -> 0.80 (-0.20)");
    assert(Line(kRows[Melee], 291, 291) == "Melee power 291");

    assert(Effect("GE_EnergyBarrier_MovementSpeedUp_C", 12.4f) == "EnergyBarrier_MovementSpeedUp 12s");
    assert(Effect("BP_GameplayEffect_AnimLock_C", -1) == "AnimLock");
    assert(Effect("GE_Regen_Heal_C", 0.2f) == "Regen_Heal 0s");

    float best = 1e9f;
    assert(!Nearer(500, 0, 0, 200, best) && best == 1e9f);  // too far from the remembered spot
    assert(Nearer(30, 40, 0, 200, best) && best == 50);
    assert(!Nearer(60, 0, 0, 200, best));                    // a farther one does not replace it
    assert(Nearer(10, 0, 0, 200, best) && best == 10);
    assert(kRowCount == Cooldown + 1);
    std::puts("ok");
}

#pragma once
#include "cast-indicator.hpp"

// Cast indicator's game-thread montage read (montage.cpp). Track: game thread only; Reset: after the listener drained.
namespace cast_montage {
    void Track();
    cast_indicator::Montage Published();  // latest published montage, any thread
    void Reset();
    int Casts();
    double Steady();      // steady clock, s (fireAt's clock)
}

#pragma once
#include "element.hpp"

// Element (core/element.hpp, from the damage-type class) → number colour.
namespace dmgnum {
    struct Rgb { float r, g, b; };

    // Physical keeps the white→gold "size" ramp; elements get a fixed hue.
    inline Rgb ColorOf(combat::Element e) {
        using combat::Element;
        switch (e) {
            case Element::Fire:        return {255, 140, 40};
            case Element::Ice:         return {130, 210, 255};
            case Element::Lightning:   return {185, 130, 255};
            case Element::Holy:        return {255, 230, 140};
            case Element::Poison:      return {170, 220, 50};
            case Element::Shadow:      return {230, 80, 200};
            case Element::Arcane:      return {110, 160, 255};
            case Element::Environment: return {190, 190, 190};
            default:                   return {255, 255, 255};
        }
    }
}

// just test
#include "cast-mode.hpp"
#include <cassert>
#include <cstdio>

using namespace cast_mode;

int main() {
    assert(SlotByte(1, false) == 0 && SlotByte(6, false) == 5 && SlotByte(1, true) == 6 && SlotByte(6, true) == 11);
    assert(SlotByte(7, false) == 6 && SlotByte(12, true) == 11);

    // which abilities aim, and how (values of the Cleric's bar, read live 2026-10-10)
    const Traits heal{2, false, false, false, false, false, 0}, channelHeal{2, true, false, false, false, false, 0};
    const Traits justice{1, false, false, false, false, false, 0}, heavenly{0, false, false, false, true, false, 0};
    const Traits holyBlast{0, false, false, false, false, true, 300}, smite{0, false, true, false, false, true, 0};
    const Traits dodge{0, false, false, false, false, false, 0}, bolt{0, false, false, false, false, true, 0};
    assert(Classify(heal, Override::Auto, true, true) == Shape::Ally);
    assert(Classify(heal, Override::Auto, false, true) == Shape::None);
    assert(Classify(channelHeal, Override::Auto, true, true) == Shape::None);    // a channel must be held
    assert(Classify(channelHeal, Override::Always, true, true) == Shape::None);  // ... whatever the menu says
    assert(Classify(justice, Override::Auto, true, true) == Shape::Enemy);
    assert(Classify(heavenly, Override::Auto, true, true) == Shape::GameReticle);
    assert(Classify(holyBlast, Override::Auto, true, true) == Shape::Ring);
    assert(Classify(smite, Override::Auto, true, true) == Shape::None);           // hold to repeat
    assert(Classify(bolt, Override::Auto, true, true) == Shape::Enemy);           // plain projectile: mark what it would hit
    assert(Classify(bolt, Override::Auto, true, false) == Shape::None);
    assert(Classify(dodge, Override::Auto, true, true) == Shape::None);
    assert(Classify(dodge, Override::Always, true, true) == Shape::Ring);
    assert(Classify(heal, Override::Never, true, true) == Shape::None);
    assert(Classify(heavenly, Override::Never, true, true) == Shape::None);

    // press an aimed ability: aim, the game sees neither the press nor its release
    State s;
    assert(Decide(s, {true, 3, Key::Other, true}) == Do::Enter && s.aiming && s.slot == 3);
    assert(Decide(s, {false, 3, Key::Other, false}) == Do::Swallow && s.aiming);
    // left click casts; the click (press + release) never reaches the game's basic attack
    assert(Decide(s, {true, 0, Key::Left, false}) == Do::Confirm && !s.aiming);
    assert(Decide(s, {false, 0, Key::Left, false}) == Do::Swallow);
    assert(Decide(s, {true, 0, Key::Left, false}) == Do::Pass);  // the next click is a normal attack again

    // right click cancels
    s = {};
    Decide(s, {true, 3, Key::Other, true});
    assert(Decide(s, {true, -1, Key::Right, false}) == Do::Cancel && !s.aiming);
    assert(Decide(s, {false, -1, Key::Right, false}) == Do::Swallow);
    assert(Decide(s, {true, -1, Key::Right, false}) == Do::Pass);

    // the same key again casts
    s = {};
    Decide(s, {true, 3, Key::Other, true});
    Decide(s, {false, 3, Key::Other, false});
    assert(Decide(s, {true, 3, Key::Other, true}) == Do::Confirm && !s.aiming);
    assert(Decide(s, {false, 3, Key::Other, false}) == Do::Swallow);

    // another ability while aiming: an aimed one takes over, a normal one cancels and goes through
    s = {};
    Decide(s, {true, 3, Key::Other, true});
    assert(Decide(s, {true, 4, Key::Other, true}) == Do::Enter && s.aiming && s.slot == 4);
    assert(Decide(s, {true, 5, Key::Other, false}) == Do::CancelPass && !s.aiming);

    // not aimed, or bound to a mouse button: untouched
    s = {};
    assert(Decide(s, {true, 2, Key::Other, false}) == Do::Pass && !s.aiming);
    assert(Decide(s, {true, 0, Key::Left, true}) == Do::Pass && !s.aiming);
    assert(Decide(s, {false, 2, Key::Other, false}) == Do::Pass);

    // a click that raises no ability event is polled
    s = {};
    assert(Click(s, Key::Left) == Do::Pass);
    Decide(s, {true, 3, Key::Other, true});
    assert(Click(s, Key::Right) == Do::Cancel && !s.aiming && s.rightHeld);
    Released(s, Key::Right);
    assert(!s.rightHeld);
    std::puts("ok");
}

// just test
#include "combat.hpp"
#include <cassert>
#include <cmath>
#include <algorithm>
#include <cstdio>

using combat::Kind;

int main() {
    combat::Tracker t;
    t.settle = 0;  // show HP drops at once: these cases test stacking, not settling
    // spawn: 0 -> full HP inside the grace window is not a heal
    t.Update({{1, 0, 0, 0, 0, false}}, 0.0);
    t.Update({{1, 0, 0, 0, 100, false}}, 0.2);
    assert(t.live.empty());
    // HP set from 0 after grace (late init) is not a heal either
    t.Update({{1, 0, 0, 0, 100, false}, {2, 0, 0, 0, 0, false}}, 0.3);
    t.Update({{1, 0, 0, 0, 100, false}, {2, 0, 0, 0, 80, false}}, 2.0);
    assert(t.live.empty());

    t.Update({{1, 0, 0, 0, 70, false}, {2, 0, 0, 0, 80, false}}, 2.1);
    assert(t.live.size() == 1 && t.live[0].kind == Kind::Dealt && t.live[0].amount == 30);
    assert(std::fabs(t.live[0].scale - 1.0f) < 1e-3);                   // first hit defines "typical"
    t.Update({{1, 0, 0, 0, 80, false}, {2, 0, 0, 0, 80, false}}, 2.2);
    assert(t.live.size() == 1 && t.live[0].amount == 20);                // rebound nets out of the stack, no heal
    t.Update({{1, 0, 0, 0, 80, false}, {2, 0, 0, 0, 80, false}, {3, 0, 0, 0, 500, true}}, 2.3);
    t.Update({{1, 0, 0, 0, 80, false}, {2, 0, 0, 0, 80, false}, {3, 0, 0, 0, 450, true}}, 4.0);
    assert(t.live.back().kind == Kind::Taken && t.fight.total == 20);    // damage taken is not DPS

    // relative size: 4x typical is bigger, tiny is smaller, clamped
    combat::Tracker s;
    assert(s.Scale(20) == 1.0f);
    assert(s.Scale(80) > 1.5f && s.Scale(1) == 0.8f && s.Scale(1e9f) <= 2.4f);   // floor 0.8: small hits stay readable
    // late game: typical drifts up, so the same 400 shrinks
    combat::Tracker late;
    late.Scale(400);
    for (int i = 0; i < 100; i++) late.Scale(20000);
    assert(late.Scale(400) == 0.8f);

    // fights: total/DPS, new fight after the gap
    combat::Tracker f;
    f.settle = 0;  // show HP drops at once: these cases test stacking, not settling
    f.Update({{9, 0, 0, 0, 1000, false}}, 0.0);
    f.Update({{9, 0, 0, 0, 900, false}}, 2.0);
    f.Update({{9, 0, 0, 0, 700, false}}, 4.0);
    assert(f.fight.total == 300 && f.fight.Dps() == 150 && f.FightActive(4.0));
    assert(!f.FightActive(9.5));
    f.Update({{9, 0, 0, 0, 650, false}}, 10.0);
    assert(f.fight.total == 50);

    // despawn + id reuse: treated as new
    combat::Tracker r;
    r.settle = 0;  // show HP drops at once: these cases test stacking, not settling
    r.Update({{1, 0, 0, 0, 100, false}}, 0.0);
    r.Update({}, 3.0);
    r.Update({{1, 0, 0, 0, 5, false}}, 3.1);
    assert(r.live.empty());
    r.Update({}, 10.0);
    assert(r.live.empty());

    // projection
    auto near = [](float a, float b) { return std::fabs(a - b) < 0.5f; };
    float sx, sy;
    combat::View v{0, 0, 0, 0, 0, 0, 90};
    assert(combat::Project(v, 100, 0, 0, 1920, 1080, sx, sy) && near(sx, 960) && near(sy, 540));
    assert(combat::Project(v, 100, 100, 0, 1920, 1080, sx, sy) && near(sx, 1920) && near(sy, 540));
    assert(combat::Project(v, 100, 0, 50, 1920, 1080, sx, sy) && near(sx, 960) && near(sy, 60));
    assert(!combat::Project(v, -100, 0, 0, 1920, 1080, sx, sy));
    combat::View turned{0, 0, 0, 0, 90, 0, 90};
    assert(combat::Project(turned, 0, 100, 0, 1920, 1080, sx, sy) && near(sx, 960));
    combat::View down{0, 0, 0, -45, 0, 0, 90};
    assert(combat::Project(down, 100, 0, -100, 1920, 1080, sx, sy) && near(sx, 960) && near(sy, 540));


    // stacking: same target within window merges, total grows, size grows
    combat::Tracker st;
    st.settle = 0;  // show HP drops at once: these cases test stacking, not settling
    st.Update({{5, 0, 0, 0, 1000, false}, {6, 0, 0, 0, 1000, false}}, 0.0);
    st.Update({{5, 0, 0, 0, 980, false}, {6, 0, 0, 0, 1000, false}}, 2.0);
    const float s1 = st.live[0].scale;
    st.Update({{5, 0, 0, 0, 960, false}, {6, 0, 0, 0, 1000, false}}, 2.5);
    st.Update({{5, 0, 0, 0, 940, false}, {6, 0, 0, 0, 990, false}}, 3.0);   // other target: separate
    assert(st.live.size() == 2 && st.live[0].amount == 60 && st.live[0].hits == 3 && st.live[0].scale > s1);
    assert(std::fabs(st.typical - 19.2f) < 0.1f && st.live[1].amount == 10);  // typical learns single hits (20,20,20,10; same-frame order free), not the 60 stack
    st.Update({{5, 0, 0, 0, 920, false}, {6, 0, 0, 0, 990, false}}, 4.35);  // 1.35 s gap > stack window: new number, old still fading
    assert(st.live.size() == 3 && st.live[2].amount == 20);
    st.Update({{5, 0, 0, 0, 920, false}, {6, 0, 0, 0, 990, false}}, 4.35 + 1.5);
    assert(st.live.empty());                                               // all expired after last bump
    {   // dip-rebound-dip (server correction) counts once
        combat::Tracker r;
        r.settle = 0;  // show HP drops at once: these cases test stacking, not settling
        combat::Sample e{9, 0, 0, 0, 100, false};
        r.Update({e}, 0.0); r.Update({e}, 2.0);
        e.health = 70; r.Update({e}, 2.1);
        e.health = 100; r.Update({e}, 2.2);
        e.health = 70; r.Update({e}, 2.3);
        assert(r.live.size() == 1 && r.live[0].kind == combat::Kind::Dealt && r.live[0].amount == 30.0f);
        assert(r.fight.total == 30.0);
        e.health = 60; r.Update({e}, 2.4);
        assert(r.live[0].amount == 40.0f);                       // a real second hit still stacks
    }
    {   // per-ability stacks; sequences as logged in game (HP drop vs LastTakeHitInfo record, render-thread sampling)
        using combat::Element;
        const uintptr_t aimed = 0xA, basic = 0xB, void_ = 0xD, poison = 0xC;
        combat::Tracker a;
        combat::Sample e{9, 0, 0, 0, 644, false};
        a.Update({e}, 0.0); a.Update({e}, 2.0);
        unsigned rep = 0;
        auto frame = [&](double t, float hp, uintptr_t type = 0, float dmg = 0, Element el = Element::Physical) {
            e.health = hp;
            if (type) { e.hitStamp = ++rep; e.hitType = type; e.element = el; e.hitDamage = dmg; }
            a.Update({e}, t);
        };
        auto near = [](float x, float y) { return std::fabs(x - y) < 0.01f; };
        // aimed shot: HP drop split over two frames, its record arrives with the second part
        frame(2.000, 623);
        assert(a.live.empty());                                       // unclaimed loss waits for its record
        frame(2.030, 493.1f, aimed, 150.9f);
        assert(a.live.size() == 1 && a.live[0].type == aimed && near(a.live[0].amount, 150.9f));
        // void proc + aimed shot in one game frame: void record (21.9) with the drop, aimed record (175 = both summed)
        // a frame later → void 21.9 and aimed 153.1, two numbers
        frame(2.500, 318.1f, void_, 21.9f, Element::Shadow);
        frame(2.525, 318.1f, aimed, 175);
        assert(a.live.size() == 2 && a.live[1].type == void_ && near(a.live[1].amount, 21.9f));
        assert(a.live[0].hits == 2 && near(a.live[0].amount, 304));
        // basic attack, record before its HP drop: own number
        frame(2.900, 318.1f, basic, 80);
        frame(2.920, 238.1f);
        assert(a.live.size() == 3 && a.live[2].type == basic && near(a.live[2].amount, 80));
        // HP drop no record claims (GAS effect): untagged, own stack, after `settle`
        frame(3.100, 228.1f);
        frame(3.150, 228.1f);
        assert(a.live.size() == 3);
        frame(3.250, 228.1f);
        assert(a.live.size() == 4 && a.live[3].type == 0 && a.live[3].element == Element::Physical && near(a.live[3].amount, 10));
        assert(near(float(a.fight.total), 644 - 228.1f));          // DPS = HP lost, attributed or not
        // poison DoT ticks 1.0 s apart: one growing number
        for (int i = 0; i < 4; i++) frame(5.0 + i, 218.1f - 10 * i, poison, 10, Element::Poison);
        assert(a.live.size() == 1 && a.live[0].type == poison && near(a.live[0].amount, 40) && a.live[0].hits == 4);
        // killing blow, record a frame late, then the actor despawns: still one aimed number
        frame(9.0, 0);
        frame(9.02, 0, aimed, 188.1f);
        a.Update({}, 9.3);
        assert(a.live.back().type == aimed && near(a.live.back().amount, 188.1f));
    }
    std::puts("ok");
}

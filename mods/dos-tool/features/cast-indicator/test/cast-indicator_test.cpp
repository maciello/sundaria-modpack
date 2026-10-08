// just test
#include "cast-indicator.hpp"
#include <cassert>
#include <cmath>
#include <cstdio>

using namespace cast_indicator;

static bool Near(float a, float b) { return std::fabs(a - b) < 1e-3f; }

int main() {
    // RapidShot (Mon_RapidShot_H_M_Crossbow2H): 8 ShootProjectile notifies + anim-lock notifies → 8 hits
    const int rapid[] = {2, 1, 1, 1, 1, 1, 1, 1, 1, 3};
    int n = 0;
    for (int t : rapid) n += IsHit(t);
    assert(n == 8);

    // game thread: 8 hits, 5 land, montage ends, late arrow lands inside the window, then done once
    Tracker tr;
    tr.Begin(8);
    for (int i = 0; i < 5; i++) tr.Hit();
    tr.End(10.0);
    assert(!tr.Tick(10.3));
    tr.Hit();
    assert(tr.c.landed == 6);
    assert(tr.Tick(10.01 + kLateHits));
    assert(tr.c.done && !tr.Tick(11.0));
    tr.Hit();
    assert(tr.c.landed == 6);  // after done nothing counts

    // all hits landed → done at once; extra hits (pierce) capped
    tr.Begin(2);
    const unsigned second = tr.c.cast;
    tr.Hit(); tr.Hit(); tr.Hit();
    assert(tr.c.landed == 2 && tr.Tick(0) && second == 2);

    // a new cast cuts an unfinished one
    tr.Begin(3);
    assert(tr.Finish() && !tr.Finish());

    // render: fill animation, punch, hold, fade
    Pips p;
    p.Update({1, 8, 0, false}, 0.0);
    assert(p.hits == 8 && p.Visible(0.0) && Near(p.Alpha(0.0), 0));
    assert(Near(p.Alpha(1.0), 1));
    p.Update({1, 8, 3, false}, 1.0);
    assert(p.Filled(2) && !p.Filled(3));
    assert(Near(p.PipScale(0, 1.0), 0.4f) && Near(p.PipScale(0, 2.0), 1));
    assert(Near(p.Flash(0, 1.0), 1) && Near(p.Flash(0, 2.0), 0));
    p.Update({1, 8, 8, true}, 2.0);
    assert(Near(p.RowScale(2.0), kPunch) && Near(p.RowScale(2.0 + kPunchDur), 1));
    assert(Near(p.Alpha(2.0 + kPunchDur + kHold), 1));
    assert(!p.Visible(2.0 + kPunchDur + kHold + style::motion::kFadeOut.dur));
    assert(!p.Muted(7));

    // ended short: empty pips muted, fade starts at done
    Pips q;
    q.Update({5, 8, 0, false}, 0.0);
    q.Update({5, 8, 6, true}, 1.0);
    assert(q.Muted(6) && q.Muted(7) && !q.Muted(5));
    assert(!q.Visible(1.0 + style::motion::kFadeOut.dur));

    // a single-hit cast is not shown, and ends the shown one
    Pips r;
    r.Update({1, 1, 0, false}, 0.0);
    assert(!r.Visible(0.0));
    r.Update({2, 4, 0, false}, 1.0);
    assert(r.Visible(1.0));
    r.Update({3, 1, 0, false}, 2.0);
    assert(r.doneAt == 2.0 && r.hits == 4);

    // row layout: centred, symmetric
    assert(Near(PipX(0, 2, 100, 1) + PipX(1, 2, 100, 1), 200));
    assert(Near(PipX(1, 3, 100, 1), 100));

    std::puts("ok");
}

// just test
#include "cast-indicator.hpp"
#include <cassert>
#include <cmath>
#include <cstdio>
#include <vector>

using namespace cast_indicator;

static bool Near(float a, float b) { return std::fabs(a - b) < 1e-3f; }

int main() {
    // RapidShot (Mon_RapidShot_H_M_Crossbow2H): 8 ShootProjectile notifies + anim-lock notifies → 8 hits
    const int rapid[] = {2, 1, 1, 1, 1, 1, 1, 1, 1, 3};
    int n = 0;
    for (int t : rapid) n += IsHit(t);
    assert(n == 8);

    // AimedShot-like: 4 sections, one shot each, no chaining → a cast of section 2 lands 1 hit
    const std::vector<Section> aimed = {{11, 0, 0.0f}, {12, 0, 1.0f}, {13, 0, 2.0f}, {14, 0, 3.0f}};
    const std::vector<Notify> shots = {{0.4f, true}, {1.4f, true}, {2.4f, true}, {3.4f, true}, {3.9f, false}};
    assert(HitTimes(aimed, shots, 4.0f, 1) == std::vector<float>{1.4f});
    assert(HitTimes(aimed, shots, 4.0f, -1).size() == 4);  // section unknown: whole montage
    // chain 0 → 1 → 2 → back to 1: sections 0..2 once
    const std::vector<Section> chain = {{11, 12, 0.0f}, {12, 13, 1.0f}, {13, 12, 2.0f}, {14, 0, 3.0f}};
    assert(HitTimes(chain, shots, 4.0f, 0).size() == 3);
    assert(Near(LinkTime(0, 1, 2, 0.5f), 0.5f) && Near(LinkTime(1, 1, 2, 0.5f), 1.5f) && Near(LinkTime(2, 1, 2, 0.5f), 2.0f));


    // wind-up: first hit 0.8 s into the section, attack speed 2 → 0.4 s; past it → fired
    const std::vector<float> one = HitTimes(aimed, shots, 4.0f, 1);
    assert(Near(FireIn(one, 1.0f, 1.0f), 0.4f));
    assert(Near(FireIn({0.8f}, 0.0f, 2.0f), 0.4f) && FireIn({0.8f}, 0.9f, 1.0f) < 0 && FireIn({}, 0, 1) < 0 && FireIn({1}, 0, 0) < 0);
    // pips: multi-hit always; a single hit only after a wind-up; spread none
    assert(PipsFor(8, 0, false) == 8 && PipsFor(1, 0.1f, false) == 0 && PipsFor(1, kWindupMin, false) == 1);
    assert(PipsFor(20, 0.5f, true) == 0 && Spread("BP_GameAbility_Salvo_C") && !Spread("BP_GameAbility_RapidShot_C"));

    // ring: 0.8 s wind-up from t=10, approach closes linearly, release at 10.8, gone after kRelease
    Ring g;
    g.Update(1, 0.8f, 10.8, false, 10.0);
    assert(g.Visible(10.0) && Near(g.Approach(10.0), kApproachR) && Near(g.Approach(10.4), (kApproachR + kRingR) / 2));
    g.Update(1, 0.8f, 10.82, false, 10.4);  // live correction before it fires
    assert(Near(float(g.fireAt), 10.82f) && !g.Fired(10.81) && g.Fired(10.82) && Near(g.Approach(10.82), kRingR));
    g.Update(1, 0.8f, 11.5, true, 10.9);  // fired: estimate frozen, a later montage end is no cancel
    assert(Near(float(g.fireAt), 10.82f) && !g.Cancelled() && Near(g.Punch(10.82), 1) && Near(g.Flash(10.82), 1));
    assert(Near(g.Punch(10.82 + kRelease), kReleasePunch) && !g.Visible(10.82 + kRelease));
    // dodge-cancel before the shot: muted fade, never fires
    g.Update(2, 0.6f, 20.6, false, 20.0);
    g.Update(2, 0.6f, 20.6, true, 20.3);
    assert(g.Cancelled() && !g.Fired(20.7) && g.Visible(20.6) && !g.Visible(20.3 + style::motion::kFadeOut.dur));
    // short wind-up: no ring
    g.Update(3, 0.1f, 30.1, false, 30.0);
    assert(!g.Visible(30.0));

    // game thread: 8 hits, 5 land, montage ends, late arrow lands inside the window, then done once
    Tracker tr;
    tr.Begin(1, 8);
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
    tr.Begin(2, 2);
    const unsigned second = tr.c.cast;
    tr.Hit(); tr.Hit(); tr.Hit();
    assert(tr.c.landed == 2 && tr.Tick(0) && second == 2);

    // a new cast cuts an unfinished one
    tr.Begin(3, 3);
    assert(tr.Finish() && !tr.Finish());

    // landed = new records on non-players by the hero; first sight and others' records do not count
    Records rec;
    std::vector<combat::Sample> w = {{1, 0, 0, 0, 100, false}, {2, 0, 0, 0, 100, false}, {3, 0, 0, 0, 100, true}};
    w[0].hitStamp = 5; w[0].hitBy = 9;
    assert(rec.New(w, 9) == 0);  // first sight
    w[0].hitStamp = 7; w[1].hitStamp = 1; w[1].hitBy = 9; w[2].hitStamp = 3; w[2].hitBy = 9;
    std::vector<const combat::Sample*> fresh;
    assert(rec.New(w, 9, &fresh) == 2 && fresh.size() == 3);  // enemy 1 + 2; the player record is not ours
    assert(rec.New(w, 9) == 0);  // same sample again
    w[0].hitStamp = 8; w[0].hitBy = 4;
    assert(rec.New(w, 9) == 0);  // someone else's hit
    assert(rec.New({}, 9) == 0 && rec.stamp.empty());

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

    // a cast without pips (ring only) is not shown, and ends the shown one; one pip (wind-up shot) is
    Pips r;
    r.Update({1, 0, 0, false}, 0.0);
    assert(!r.Visible(0.0));
    r.Update({2, 4, 0, false}, 1.0);
    assert(r.Visible(1.0));
    r.Update({3, 0, 0, false}, 2.0);
    assert(r.doneAt == 2.0 && r.hits == 4);
    r.Update({4, 1, 0, false}, 3.0);
    assert(r.Visible(3.0) && r.hits == 1);

    // row layout: centred, symmetric
    assert(Near(PipX(0, 2, 100, 1) + PipX(1, 2, 100, 1), 200));
    assert(Near(PipX(1, 3, 100, 1), 100));

    std::puts("ok");
}

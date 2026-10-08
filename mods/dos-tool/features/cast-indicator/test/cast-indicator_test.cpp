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

    assert(Near(LinkTime(0, 1, 2, 0.5f), 0.5f) && Near(LinkTime(1, 1, 2, 0.5f), 1.5f) && Near(LinkTime(2, 1, 2, 0.5f), 2.0f));

    // Montages from the game (just data show, notify dump 2026-10-09). AimedShot: 4 sections, one per cast, no chaining.
    Timeline aimed{{{11, 0, 0.0f}, {12, 0, 1.5f}, {13, 0, 3.0f}, {14, 0, 4.5f}},
                   {{0.88f, true}, {1.26f, false}, {2.36f, true}, {3.85f, true}, {5.36f, true}, {5.86f, false}}, 6.0f};
    assert(aimed.Hits(1) == std::vector<float>{2.36f} && aimed.Hits(-1).size() == 4);  // unknown section: whole montage
    assert(Near(aimed.Ahead(1, 1.5f, 1.0f).in, 0.86f) && !aimed.Ahead(1, 1.5f, 1.0f).held);
    assert(Near(aimed.Ahead(1, 1.5f, 0.85f).in, 0.86f / 0.85f));  // play rate = attack speed
    assert(aimed.Ahead(1, 2.5f, 1.0f).in < 0 && aimed.Ahead(1, 0, 0).in < 0);  // fired / no rate
    // chain 0 → 1 → 2 → back to 1: sections 0..2 once
    Timeline chain{{{11, 12, 0.0f}, {12, 13, 1.0f}, {13, 12, 2.0f}, {14, 0, 3.0f}},
                   {{0.4f, true}, {1.4f, true}, {2.4f, true}, {3.4f, true}, {3.9f, false}}, 4.0f};
    assert(chain.Hits(0).size() == 3 && Near(chain.Ahead(0, 0.5f, 1.0f).in, 0.9f));  // into the next section

    // DeadlyAim / PoisonArrow / Hemlock (ShootArrow, CanHold): Pull → Hold (loops, no hit) → release jumps to Shoot (2.40).
    // Starting in Pull, the chain alone never reaches Shoot: that was the missing cast line.
    Timeline hold{{{1, 2, 0.0f}, {2, 2, 1.0667f}, {3, 0, 2.4f}}, {{2.40f, true}, {2.73f, false}}, 3.4f};
    assert(hold.Hits(0).empty() && hold.Hits(0, 2) == std::vector<float>{2.40f});
    assert(hold.Hold(1) && !hold.Hold(0) && !hold.Hold(2));
    const Timeline::Aim pull = hold.Ahead(0, 0.0f, 1.0f, 2);  // drawing: armed when Pull ends (a tap fires then)
    assert(Near(pull.in, 1.0667f) && !pull.held);
    const Timeline::Aim held = hold.Ahead(1, 1.5f, 1.0f, 2);  // holding: fires on release
    assert(Near(held.in, 0) && held.held);
    assert(hold.Crossed(2, hold.Begin(2) - 1e-3f, 2.41f) == 1 && hold.Crossed(2, 2.41f, 2.8f) == 0);  // release jump
    assert(hold.Crossed(1, 1.0f, 2.3f) == 0);
    // ParalyzingShot: one section, CanHold but no Hold/Release section: a plain 1.07 s wind-up
    Timeline para{{{5, 0, 0.0f}}, {{1.07f, true}, {1.50f, false}}, 2.0667f};
    assert(para.Hits(0, -1).size() == 1 && Near(para.Ahead(0, 0, 1.07f).in, 1.0f));
    // RapidShot: 8 arrows every 0.2 s; notifies crossed between ticks count once each
    Timeline rapidTl{{{7, 0, 0.0f}}, {{0.01f, true}, {0.20f, true}, {0.39f, true}, {0.60f, true}, {0.80f, true}, {1.01f, true},
                                   {1.20f, true}, {1.40f, true}, {1.48f, false}}, 1.65f};
    assert(rapidTl.Crossed(0, -1e-3f, 0.016f) == 1 && rapidTl.Crossed(0, 0.016f, 0.45f) == 2 && rapidTl.Crossed(0, 0.45f, 1.65f) == 5);

    // pips: multi-hit always; a single hit only after a wind-up; spread none
    assert(PipsFor(8, 0, false) == 8 && PipsFor(1, 0.1f, false) == 0 && PipsFor(1, kWindupMin, false) == 1);
    assert(PipsFor(20, 0.5f, true) == 0 && Spread("BP_GameAbility_Salvo_C") && !Spread("BP_GameAbility_RapidShot_C"));

    // ring: 0.8 s wind-up from t=10, approach closes linearly, meets at 10.8 (armed until the notify passes), fired at 10.83
    Ring g;
    g.Update(1, 0.8f, 10.8, false, false, false, 10.0);
    assert(g.Visible(10.0) && Near(g.Approach(10.0), kApproachR) && Near(g.Approach(10.4), (kApproachR + kRingR) / 2));
    g.Update(1, 0.8f, 10.82, false, false, false, 10.4);  // live correction
    assert(Near(float(g.fireAt), 10.82f) && !g.Armed(10.81) && g.Armed(10.82) && Near(g.Approach(10.82), kRingR));
    g.Update(1, 0.8f, 10.82, false, true, false, 10.83);  // notify passed
    assert(g.Fired() && !g.Armed(10.83) && Near(g.Punch(10.83), 1) && Near(g.Flash(10.83), 1));
    g.Update(1, 0.8f, 11.5, false, true, true, 10.9);  // frozen; a later montage end is no cancel
    assert(!g.Cancelled() && Near(g.Punch(10.83 + kRelease), kReleasePunch) && !g.Visible(10.83 + kRelease));
    // hold: armed at once while held, stays until the release fires it
    g.Update(4, 1.07f, 41.0, false, false, false, 40.0);
    g.Update(4, 1.07f, 45.0, true, false, false, 42.0);
    assert(g.Armed(42.0) && Near(g.Approach(42.0), kRingR) && g.Visible(44.0));
    g.Update(4, 1.07f, 45.0, true, true, true, 45.0);  // release: arrow out and the ability ends in one tick
    assert(g.Fired() && !g.Cancelled());
    // dodge-cancel before the shot: muted fade, never fires
    g.Update(2, 0.6f, 20.6, false, false, false, 20.0);
    g.Update(2, 0.6f, 20.6, false, false, true, 20.3);
    assert(g.Cancelled() && !g.Fired() && !g.Armed(20.7) && g.Visible(20.6) && !g.Visible(20.3 + style::motion::kFadeOut.dur));
    // short wind-up: no ring
    g.Update(3, 0.1f, 30.1, false, false, false, 30.0);
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

    // fired, then hit: a shot that misses stays "fired" (never empty), a hit upgrades it; muted only if never shot
    Pips fp;
    fp.Update({6, 3, 0, false, 0}, 0.0);
    fp.Update({6, 3, 0, false, 2}, 0.5);
    assert(fp.Fired(0) && fp.Fired(1) && !fp.Fired(2) && !fp.Filled(0) && Near(fp.PipScale(0, 0.5), 0.4f));
    fp.Update({6, 3, 1, false, 2}, 0.8);
    assert(fp.Filled(0) && !fp.Fired(0) && fp.Fired(1) && Near(fp.Flash(0, 0.8), 1) && Near(fp.Flash(1, 0.8), 0));
    fp.Update({6, 3, 1, true, 2}, 2.0);
    assert(fp.Fired(1) && !fp.Muted(1) && fp.Muted(2));
    // tracker: fired counts are capped and stop once done
    Tracker ft;
    ft.Begin(7, 2);
    ft.Fire(1); ft.Fire(5);
    assert(ft.c.fired == 2);

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

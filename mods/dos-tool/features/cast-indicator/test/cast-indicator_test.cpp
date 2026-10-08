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
    assert(HitsFrom(aimed, shots, 4.0f, 1) == 1);
    assert(HitsFrom(aimed, shots, 4.0f, -1) == 4);  // section unknown: whole montage
    // chain 0 → 1 → 2 → back to 1: sections 0..2 once
    const std::vector<Section> chain = {{11, 12, 0.0f}, {12, 13, 1.0f}, {13, 12, 2.0f}, {14, 0, 3.0f}};
    assert(HitsFrom(chain, shots, 4.0f, 0) == 3);
    assert(Near(LinkTime(0, 1, 2, 0.5f), 0.5f) && Near(LinkTime(1, 1, 2, 0.5f), 1.5f) && Near(LinkTime(2, 1, 2, 0.5f), 2.0f));

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

    // pip weight: final shot does double → capped 1.4, others 1; unseen key or index → 1
    Weights wt;
    for (int i = 0; i < 4; i++) wt.Learn("A", 4, i, i == 3 ? 200.0f : 100.0f);
    assert(Near(wt.Of("A", 0), 1) && Near(wt.Of("A", 3), kWeightMax) && Near(wt.Of("B", 0), 1));
    wt.Learn("A", 4, 3, 100.0f);  // EMA: 200 + 0.3 × (100 - 200) = 170 → 1.7× median, still capped
    assert(Near(wt.dmg["A"][3], 170) && Near(wt.Of("A", 3), kWeightMax));
    std::vector<float> dmg;
    w[0].hitStamp = 9; w[0].hitBy = 9; w[0].hitDamage = 42;
    assert(rec.New(w, 9, nullptr, &dmg) == 0);  // rec was emptied above: first sight again
    w[0].hitStamp = 10;
    assert(rec.New(w, 9, nullptr, &dmg) == 1 && dmg.size() == 1 && dmg[0] == 42);

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

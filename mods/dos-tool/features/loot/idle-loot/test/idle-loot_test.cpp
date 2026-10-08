// just test
#include "idle-loot.hpp"
#include <cassert>
#include <cmath>
#include <cstdio>
#include <set>

int main() {
    using namespace idle_loot;
    // schedule: irregular gaps inside [kGapMin, kGapMax], first burst within kGapMax; piles out of sync
    std::set<int> gaps;
    float sum = 0;
    for (std::uint32_t n = 1; n < 200; n++) {
        const float g = Gap(0xABCDEF10ull, n);
        assert(g >= kGapMin && g <= kGapMax);
        gaps.insert(int(g * 10));
        sum += g;
    }
    assert(gaps.size() > 20);                                          // random, not a steady pulse
    assert(sum / 199 > kGapMin + 1 && sum / 199 < kGapMax - 1);         // mean ~ middle: low frequency
    assert(Gap(0x1000, 0) >= 0 && Gap(0x1000, 0) < kGapMax && Gap(0x1000, 0) != Gap(0x1008, 0));
    // bursts: many tiny motes, each lives once inside the burst, rises, stays near the anchor
    for (std::uint32_t b = 0; b < 50; b++) {
        const int n = Motes(0x77, b);
        assert(n >= kMotesMin && n <= kMotesMax);
        for (int k = 0; k < n; k++) {
            float alive = 0, peak = 0, y0 = 1e9f, y1 = 0;
            for (float t = 0; t < kBurstLen + 0.5f; t += 0.002f) {
                const Mote m = MoteAt(0x77, b, k, t);
                assert(m.a >= 0 && m.a <= 1.0001f);
                if (m.a <= 0) continue;
                alive += 0.002f;
                peak = std::max(peak, m.a);
                assert(m.r <= (m.glint ? kGlintR : kMoteMaxR) + 1e-4f);
                assert(std::fabs(m.dx) <= kSpreadX + kDrift * kLifeMax + 1e-3f);
                if (y0 > 1e8f) y0 = m.dy;
                y1 = m.dy;
            }
            assert(alive >= kLifeMin - 0.01f && alive <= kLifeMax + 0.01f && peak > 0.98f);
            assert(y1 < y0);  // rises (y down)
            assert(MoteAt(0x77, b, k, kBurstLen).a == 0);
        }
    }
    // piles: best grade wins (purple Epic 4 over grey Poor 0 and unknown -1), far loot is its own pile, looted ignored
    using loot::Actor, loot::Kind;
    std::vector<Actor> list = {
        {30, Kind::Item, 0, 0, 0, true, 0, NAN},    {10, Kind::Item, 50, 0, 0, true, 4, NAN},
        {20, Kind::Item, 0, 80, 0, true, -1, NAN},  {40, Kind::Item, 2000, 0, 0, true, 2, NAN},
        {50, Kind::Item, 10, 10, 0, false, 7, NAN}, {60, Kind::Chest, 2000, 100, 0, true, -1, 5.0f},
    };
    std::vector<Pile> piles;
    Piles(list, 10, piles);
    assert(piles.size() == 2);
    assert(piles[0].key == 10 && piles[0].grade == 4 && piles[0].members == 3 && !piles[0].chest && piles[0].onScreen);
    assert(piles[1].key == 40 && piles[1].grade == 2 && piles[1].members == 2 && piles[1].chest && piles[1].onScreen);
    Piles({{60, Kind::Chest, 0, 0, 0, true, -1, 5.0f}}, 10, piles);  // its only mesh behind a wall
    assert(piles.size() == 1 && !piles[0].onScreen && piles[0].grade == -1);
    // distance: full until 25 m, gone at 30 m; depth clamped
    assert(DistFade(1000) == 1 && DistFade(kCull) == 0 && DistFade(2750) > 0.4f && DistFade(2750) < 0.6f);
    assert(Depth(100) == 1.25f && Depth(10000) == 0.6f);
    // hidden: alpha reaches 0 within kFadeOut (<= 1 s); fade in over kFadeIn
    float a = 1;
    for (int i = 0; i < 60 && a > 0; i++) a = Approach(a, 0, 1 / 60.f);
    assert(a == 0 && style::motion::kFadeOut.dur <= 1);
    assert(Approach(0, 1, style::motion::kFadeIn.dur) == 1);
    // occlusion: rendered recently = on screen; unknown = on screen
    assert(OnScreen(9.95f, 10) && !OnScreen(9.5f, 10) && OnScreen(NAN, 10) && OnScreen(-1000, 0));
    std::puts("idle-loot ok");
}

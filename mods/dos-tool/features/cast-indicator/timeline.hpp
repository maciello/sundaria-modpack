#pragma once
#include <algorithm>
#include <cstdint>
#include <vector>

// Cast indicator: the playing montage as plain data (sections, notifies) and what a cast does with it. SDK-free.
// Hold abilities (UBP_GameAbility_ShootArrow_C, CanHold; game-facts.md § ability_hits.hold): Pull → Hold (NextSection =
// itself, no hits: waits for the release) → on release the ability jumps to ReleaseAnimInfo's section (Shoot), whose
// ShootProjectile notify fires. A tap during Pull sets Shoot as Pull's next section instead.
namespace cast_indicator {
    // FAnimLinkableElement::GetTime: LinkMethod Absolute 0, Relative 1, Proportional 2 (UE 4.27).
    inline float LinkTime(int method, float segBegin, float segLen, float value) {
        return method == 1 ? segBegin + value : method == 2 ? segBegin + segLen * value : value;
    }

    // Montage sections: names are FName ids (0 = None). A cast plays its start section, then follows NextSectionName.
    struct Section { std::uint64_t name, next; float time; };
    struct Notify { float time; bool hit; };

    struct Timeline {
        std::vector<Section> secs;
        std::vector<Notify> ns;
        float length = 0;

        int Find(std::uint64_t name) const {
            for (int i = 0; name && i < int(secs.size()); i++) if (secs[i].name == name) return i;
            return -1;
        }
        bool Valid(int i) const { return i >= 0 && i < int(secs.size()); }
        float Begin(int i) const { return Valid(i) ? secs[i].time : 0; }
        float End(int i) const {  // unknown section = the whole montage
            if (!Valid(i)) return length;
            float e = length;
            for (const Section& o : secs) if (o.time > secs[i].time) e = std::min(e, o.time);
            return e;
        }
        int Next(int i) const { return Valid(i) ? Find(secs[i].next) : -1; }
        // Hit notify times in section i at or after `from`, sorted.
        std::vector<float> HitsIn(int i, float from = -1e9f) const {
            std::vector<float> t;
            for (const Notify& x : ns) if (x.hit && x.time >= Begin(i) && x.time < End(i) && x.time >= from) t.push_back(x.time);
            std::sort(t.begin(), t.end());
            return t;
        }
        // A hold loop: the section loops into itself without a hit; the cast waits there for the release.
        bool Hold(int i) const { return Valid(i) && secs[i].next == secs[i].name && HitsIn(i).empty(); }

        // Times of the hits the cast should land: the start section and the sections it chains into (each once). A chain
        // that ends in a hold loop fires the release section's hits. start < 0 (unknown section) = the whole montage.
        std::vector<float> Hits(int start, int release = -1) const {
            if (!Valid(start)) return HitsIn(-1);
            std::vector<float> t;
            std::vector<bool> seen(secs.size());
            bool held = false;
            for (int i = start; Valid(i) && !seen[i]; i = Next(i)) {
                seen[i] = true;
                held = held || Hold(i);
                for (float x : HitsIn(i)) t.push_back(x);
            }
            if (held && Valid(release) && !seen[release]) for (float x : HitsIn(release)) t.push_back(x);
            std::sort(t.begin(), t.end());
            return t;
        }

        // When the next hit fires, seen from section `cur` at montage position `pos`, play rate `rate`.
        // in = real s until it (< 0 = no hit ahead); held = the cast waits in the hold loop (fires on release, in s after it).
        struct Aim { float in; bool held; };
        Aim Ahead(int cur, float pos, float rate, int release = -1) const {
            if (rate <= 0) return {-1, false};
            const float onRelease = Valid(release) && !HitsIn(release).empty() ? HitsIn(release).front() - Begin(release) : 0;
            float acc = 0;  // montage s until section i begins
            std::vector<bool> seen(secs.size());
            for (int i = cur; ; ) {
                const std::vector<float> h = HitsIn(i, pos);
                if (!h.empty()) return {(acc + h.front() - pos) / rate, false};
                if (Hold(i)) return {(acc + onRelease) / rate, i == cur};
                if (!Valid(i)) return {-1, false};
                seen[i] = true;
                acc += End(i) - pos;
                i = Next(i);
                if (!Valid(i) || seen[i]) return {-1, false};
                pos = Begin(i);
            }
        }

        // Hit notifies of section `cur` passed when the position moved from `from` (exclusive) to `pos` (inclusive).
        int Crossed(int cur, float from, float pos) const {
            int n = 0;
            for (float t : HitsIn(cur)) n += t > from && t <= pos;
            return n;
        }
    };
}

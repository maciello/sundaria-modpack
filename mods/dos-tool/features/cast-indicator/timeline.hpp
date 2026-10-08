#pragma once
#include <algorithm>
#include <cstdint>
#include <vector>

// Cast indicator: the playing montage as plain data (sections, notifies) and what a cast does with it. SDK-free.
namespace cast_indicator {
    // FAnimLinkableElement::GetTime: LinkMethod Absolute 0, Relative 1, Proportional 2 (UE 4.27).
    inline float LinkTime(int method, float segBegin, float segLen, float value) {
        return method == 1 ? segBegin + value : method == 2 ? segBegin + segLen * value : value;
    }

    // Montage sections: names are FName ids (0 = None). A cast plays its start section, then follows NextSectionName.
    struct Section { std::uint64_t name, next; float time; };
    struct Notify { float time; bool hit; };

    // Times (montage s, sorted) of the hits the cast should land: hit notifies in the start section and the sections it
    // chains into (each once: a loop back counts once). start < 0 (unknown section) = the whole montage.
    inline std::vector<float> HitTimes(const std::vector<Section>& secs, const std::vector<Notify>& ns, float length, int start) {
        std::vector<float> t;
        if (start < 0 || start >= int(secs.size())) {
            for (const Notify& x : ns) if (x.hit) t.push_back(x.time);
            std::sort(t.begin(), t.end());
            return t;
        }
        std::vector<bool> seen(secs.size());
        for (int i = start; i >= 0 && !seen[i];) {
            seen[i] = true;
            float end = length;
            for (const Section& o : secs) if (o.time > secs[i].time) end = std::min(end, o.time);
            for (const Notify& x : ns) if (x.hit && x.time >= secs[i].time && x.time < end) t.push_back(x.time);
            const std::uint64_t next = secs[i].next;
            i = -1;
            for (int j = 0; next && j < int(secs.size()); j++) if (secs[j].name == next) { i = j; break; }
        }
        std::sort(t.begin(), t.end());
        return t;
    }

    // Wind-up (#92): real seconds from montage position `pos` to the first hit at play rate `rate`; < 0 = fired or none.
    inline float FireIn(const std::vector<float>& hits, float pos, float rate) {
        return hits.empty() || rate <= 0 ? -1 : (hits.front() - pos) / rate;
    }
}

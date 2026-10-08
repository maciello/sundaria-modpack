#pragma once
// SDK-free: levers that open the way (#93): the order the main route visits them in.
#include <algorithm>
#include <cmath>
#include <vector>
#include "path.hpp"

namespace dungeon_map {
    // Nearest first, from `from` on. ponytail: greedy; fine for the 1–3 levers one door needs.
    inline std::vector<V3> LeverOrder(V3 from, std::vector<V3> ls) {
        std::vector<V3> out;
        while (!ls.empty()) {
            auto it = std::min_element(ls.begin(), ls.end(), [&](V3 a, V3 b) { return Dist(a, from) < Dist(b, from); });
            from = *it;
            out.push_back(*it);
            ls.erase(it);
        }
        return out;
    }
}

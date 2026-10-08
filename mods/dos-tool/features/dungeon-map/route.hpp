#pragma once
// SDK-free: the way from the player to the floor's goal (#83). Navmesh path from here; where it stops short (locked door,
// navmesh island), resume at a navmesh point in the next room of the floor's room chain (ChunkActors order: entry …
// stairs down; static floors have no other room graph, game-facts.md § Dungeon) and ask again. O(rooms) queries.
#include <algorithm>
#include <functional>
#include <vector>
#include "path.hpp"

namespace dungeon_map {
    // Distance from p to box b (0 inside).
    inline float BoxDist(const Box& b, V3 p) {
        const float a = -b.yawDeg * 3.14159265f / 180.f, dx = p.x - b.c.x, dy = p.y - b.c.y;
        const float lx = dx * std::cos(a) - dy * std::sin(a), ly = dx * std::sin(a) + dy * std::cos(a);
        const float ox = std::max(std::abs(lx) - b.e.x, 0.f), oy = std::max(std::abs(ly) - b.e.y, 0.f), oz = std::max(std::abs(p.z - b.c.z) - b.e.z, 0.f);
        return std::sqrt(ox * ox + oy * oy + oz * oz);
    }
    // Room nearest p (corridors between boxes count as the nearest room), -1 = no rooms.
    inline int NearestRoom(const std::vector<Box>& rooms, V3 p) {
        int best = -1;
        float bd = INFINITY;
        for (int i = 0; i < int(rooms.size()); i++)
            if (const float d = BoxDist(rooms[i], p); d < bd) { bd = d; best = i; }
        return best;
    }

    struct Nav {
        std::function<bool(V3 a, V3 b, Path& out, bool& partial)> path;  // the game's navmesh query
        std::function<bool(const Box& room, V3& out)> anchor;            // a navmesh point in the room, false = none
    };
    struct Route {
        Path path;              // from → goal, always ends at the goal when there is one
        std::vector<V3> stops;  // where a navmesh leg stopped short; the route jumps on from there
        int legs = 0;
    };

    inline void Append(Path& p, V3 v) { if (p.empty() || Dist(p.back(), v) > 1) p.push_back(v); }

    inline Route PlanRoute(V3 from, V3 goal, const std::vector<Box>& rooms, const Nav& nav) {
        Route r;
        auto add = [&](V3 v) { Append(r.path, v); };
        V3 a = from;
        int at = NearestRoom(rooms, from);  // never resume behind the player
        for (size_t guard = 0; guard <= rooms.size(); guard++) {
            Path leg;
            bool partial = false;
            r.legs++;
            if (!nav.path(a, goal, leg, partial)) leg = {a}, partial = true;  // no navmesh at a
            for (const V3& v : leg) add(v);
            if (!partial) return r;
            const V3 end = r.path.back();
            r.stops.push_back(end);
            int k = std::max(at, NearestRoom(rooms, end)) + 1;
            V3 b;
            while (k < int(rooms.size()) && !nav.anchor(rooms[k], b)) k++;
            if (k >= int(rooms.size())) break;
            add(b);  // straight across what the navmesh does not connect
            a = b;
            at = k;
        }
        add(goal);  // ponytail: straight to the goal past the last room; a crumb graph would route generated floors
        return r;
    }

    // Through the waypoints in order (levers that open the way, #93), then on to the goal. A waypoint the navmesh does
    // not reach is joined straight.
    inline Route PlanVia(V3 from, const std::vector<V3>& via, V3 goal, const std::vector<Box>& rooms, const Nav& nav) {
        Route r;
        V3 a = from;
        for (const V3& w : via) {
            Path leg;
            bool partial = false;
            r.legs++;
            if (nav.path(a, w, leg, partial)) for (const V3& v : leg) Append(r.path, v);
            Append(r.path, a = w);
        }
        const Route rest = PlanRoute(a, goal, rooms, nav);
        for (const V3& v : rest.path) Append(r.path, v);
        r.stops = rest.stops;
        r.legs += rest.legs;
        return r;
    }
}

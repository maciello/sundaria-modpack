#pragma once
// SDK-free: the way from the player to the floor's goal (#83). Navmesh path from here toward the goal; where it stops
// short, aim at a navmesh point in the next room of the floor's room chain (ChunkActors order: entry … stairs down; static
// floors have no other room graph, game-facts.md § Dungeon). Reached: on toward the goal (the long query ran out of search
// nodes). Not reached (locked door, navmesh island): a stop, the route jumps straight to that point. O(rooms) queries.
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
        Path path;                  // from → goal, always ends at the goal when there is one
        std::vector<V3> stops;      // where a navmesh leg stopped short; the route jumps on from there
        std::vector<size_t> stopAt; // index of each stop in path
        int legs = 0;
    };

    inline void Append(Path& p, V3 v) { if (p.empty() || Dist(p.back(), v) > 1) p.push_back(v); }

    // The route, one navmesh leg per Step, so a plan spreads over world ticks (a partial query can cost ms).
    struct RouteJob {
        Route r;
        bool done = false;
        void Start(V3 from, V3 goal_, std::vector<Box> rooms_) {
            *this = {};
            goal = goal_, rooms = std::move(rooms_), a = from;
            at = NearestRoom(rooms, from);  // never resume behind the start
        }
        bool Step(const Nav& nav) {  // true = done
            if (done) return true;
            Path leg;
            bool partial = false;
            r.legs++;
            if (!nav.path(a, aiming ? b : goal, leg, partial)) leg = {a}, partial = true;  // no navmesh at a
            for (const V3& v : leg) Append(r.path, v);
            if (!partial && !aiming) return done = true;
            if (aiming) {  // toward the next room: reached, else a stop and straight across what the navmesh does not connect
                if (partial) Stop(), Append(r.path, b);
                a = b, aiming = false;
                return false;
            }
            int k = std::max(at, NearestRoom(rooms, r.path.back())) + 1;
            while (k < int(rooms.size()) && !nav.anchor(rooms[k], b)) k++;
            if (k >= int(rooms.size())) {
                Stop(), Append(r.path, goal);  // ponytail: straight to the goal past the last room; a crumb graph would route generated floors
                return done = true;
            }
            a = r.path.back(), at = k, aiming = true;
            return false;
        }
    private:
        void Stop() { r.stops.push_back(r.path.back()), r.stopAt.push_back(r.path.size() - 1); }
        V3 goal, a, b;
        std::vector<Box> rooms;
        int at = -1;
        bool aiming = false;  // the current leg goes to b (next room's navmesh point), not the goal
    };

    inline Route PlanRoute(V3 from, V3 goal, const std::vector<Box>& rooms, const Nav& nav) {
        RouteJob j;
        j.Start(from, goal, rooms);
        while (!j.Step(nav)) {}
        return j.r;
    }

    // A detour from a stop through waypoints (levers that open the way, #93) and back to the stop, one leg per Step.
    // A waypoint the navmesh does not reach is joined straight.
    struct DetourJob {
        Path path;
        int legs = 0;
        void Start(V3 stop, std::vector<V3> via) {
            *this = {};
            to = std::move(via), to.push_back(stop), a = stop;
            Append(path, stop);
        }
        bool Done() const { return next >= to.size(); }
        bool Step(const Nav& nav) {  // true = done
            if (Done()) return true;
            Path leg;
            bool partial = false;
            legs++;
            if (nav.path(a, to[next], leg, partial)) for (const V3& v : leg) Append(path, v);
            Append(path, a = to[next++]);
            return Done();
        }
    private:
        std::vector<V3> to;
        size_t next = 0;
        V3 a;
    };

    // p with the detour put in at index i (where it starts and ends).
    inline Path Splice(const Path& p, size_t i, const Path& detour) {
        Path out(p.begin(), p.begin() + std::min(i, p.size()));
        for (const V3& v : detour) Append(out, v);
        for (size_t j = i + 1; j < p.size(); j++) Append(out, p[j]);
        return out;
    }
}

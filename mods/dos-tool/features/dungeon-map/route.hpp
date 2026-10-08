#pragma once
// SDK-free logic of the dungeon map (#40): the main path through one floor's room graph and the local player's frontier.
// Rooms = the game's slices, doors = the crumbs that join two slices. World units (cm), XY only.
#include <cmath>
#include <queue>
#include <vector>

namespace dungeon_map {
    struct P { float x = 0, y = 0; };
    inline float Dist(P a, P b) { return std::hypot(a.x - b.x, a.y - b.y); }

    struct Door { int to; P at; };
    struct Room { P c; std::vector<Door> doors; };
    using Graph = std::vector<Room>;

    inline void Link(Graph& g, int a, int b, P at) {
        g[a].doors.push_back({b, at});
        g[b].doors.push_back({a, at});
    }

    // Shortest walk from room `from` to room `to` (centre → door → centre lengths): room indices, empty if unreachable.
    inline std::vector<int> Route(const Graph& g, int from, int to) {
        const int n = int(g.size());
        if (from < 0 || to < 0 || from >= n || to >= n) return {};
        std::vector<float> d(n, INFINITY);
        std::vector<int> prev(n, -1);
        using Q = std::pair<float, int>;
        std::priority_queue<Q, std::vector<Q>, std::greater<Q>> q;
        d[from] = 0;
        q.push({0, from});
        while (!q.empty()) {
            const auto [du, u] = q.top();
            q.pop();
            if (du > d[u]) continue;
            for (const Door& e : g[u].doors) {
                const float w = du + Dist(g[u].c, e.at) + Dist(e.at, g[e.to].c);
                if (w < d[e.to]) d[e.to] = w, prev[e.to] = u, q.push({w, e.to});
            }
        }
        if (d[to] == INFINITY) return {};
        std::vector<int> r;
        for (int v = to; v != -1; v = prev[v]) r.insert(r.begin(), v);
        return r;
    }

    // The door between neighbours a and b (the first one if several).
    inline P DoorBetween(const Graph& g, int a, int b) {
        for (const Door& e : g[a].doors)
            if (e.to == b) return e.at;
        return g[b].c;
    }

    // The drawn line: start point, each door along the route, then the centre of route[reach] (reach = frontier index).
    inline std::vector<P> Line(const Graph& g, const std::vector<int>& route, int reach, P start) {
        std::vector<P> out;
        if (route.empty() || reach < 0) return out;
        out.push_back(start);
        for (int i = 1; i <= reach && i < int(route.size()); i++) out.push_back(DoorBetween(g, route[i - 1], route[i]));
        out.push_back(g[route[std::min(reach, int(route.size()) - 1)]].c);
        return out;
    }

    // The local player's frontier on one floor: the furthest route index among rooms they stood in. Never moves back:
    // a room counts once visited, and a new route (graph change) keeps the reach unless it lies further.
    struct Frontier {
        std::vector<bool> visited;  // per room
        int reach = -1;             // index into the route, -1 = no route room visited yet

        // Player stood in `room`; true = the frontier advanced.
        bool Visit(int room, const std::vector<int>& route) {
            if (room < 0) return false;
            if (room >= int(visited.size())) visited.resize(room + 1, false);
            visited[room] = true;
            return Rescan(route);
        }
        // Route changed: furthest visited index on it, if further than before.
        bool Rescan(const std::vector<int>& route) {
            int r = reach;
            for (int i = int(route.size()) - 1; i > r; i--)
                if (route[i] < int(visited.size()) && visited[route[i]]) { r = i; break; }
            if (r == reach) return false;
            reach = r;
            return true;
        }
    };

    // Room holding point p: the one whose box (centre c, half extents e, axis-aligned in its own yaw) contains it.
    // Callers test the current room and its neighbours only (O(doors)); a full scan once per floor.
    struct Box { P c, e; float yawDeg; float zc, ze; };
    inline bool Inside(const Box& b, P p, float z) {
        const float a = -b.yawDeg * 3.14159265f / 180.f, dx = p.x - b.c.x, dy = p.y - b.c.y;
        const float lx = dx * std::cos(a) - dy * std::sin(a), ly = dx * std::sin(a) + dy * std::cos(a);
        return std::abs(lx) <= b.e.x && std::abs(ly) <= b.e.y && std::abs(z - b.zc) <= b.ze;
    }
}

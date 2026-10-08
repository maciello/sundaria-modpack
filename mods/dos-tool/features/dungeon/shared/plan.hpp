#pragma once
// The current floor's main route, entry → exit, and the connector from the player to it (plan.cpp reads the dungeon and
// asks the game's navmesh, route.hpp).
// SDK-free types and choices; facts: references/game-facts.md § Dungeon.
#include <string>
#include <vector>
#include "path.hpp"

namespace dungeon_map {
    constexpr float kDoorNear = 1500;  // a navmesh leg that a door cuts ends this close to it

    struct Marks {
        bool locked = false;  // path stops at a locked door
        V3 door;
        std::vector<V3> levers;  // heuristic: unpulled levers in that door's room; the route runs through them
    };

    struct Trigger {
        V3 at;
        int room = -1;  // index into Plan::rooms, -1 = none
        bool door = false, lever = false, closed = false, locked = false;
    };

    // The closed door nearest the end of a navmesh leg that stopped short, -1 = none within kDoorNear.
    inline int StoppingDoor(const std::vector<Trigger>& ts, V3 end) {
        int best = -1;
        float bd = kDoorNear;
        for (int i = 0; i < int(ts.size()); i++)
            if (ts[i].door && ts[i].closed)
                if (const float d = Dist(ts[i].at, end); d <= bd) { bd = d; best = i; }
        return best;
    }

    // Doors near where a leg stopped, for the log (#83): counts and nearest distances, -1 = none.
    struct DoorGap { int doors = 0, closed = 0; float nearestClosed = -1, nearestDoor = -1; };
    inline DoorGap DoorsAround(const std::vector<Trigger>& ts, V3 end) {
        DoorGap g;
        for (const Trigger& t : ts) {
            if (!t.door) continue;
            const float d = Dist(t.at, end);
            g.doors++;
            if (g.nearestDoor < 0 || d < g.nearestDoor) g.nearestDoor = d;
            if (!t.closed) continue;
            g.closed++;
            if (g.nearestClosed < 0 || d < g.nearestClosed) g.nearestClosed = d;
        }
        return g;
    }

    // Marks for a path that stopped at door i. Levers: ponytail heuristic (#68): no data links a lever to its door, so
    // every unpulled lever in the door's room is marked; replace with the real link once a probe finds one.
    inline Marks MarksFor(const std::vector<Trigger>& ts, int i) {
        Marks m;
        if (i < 0 || !ts[i].locked) return m;
        m.locked = true;
        m.door = ts[i].at;
        for (const Trigger& t : ts)
            if (t.lever && t.closed && t.room >= 0 && t.room == ts[i].room) m.levers.push_back(t.at);
        return m;
    }

    // Room of point p, -1 = none.
    inline int RoomOf(const std::vector<Box>& rooms, V3 p) {
        for (int i = 0; i < int(rooms.size()); i++) if (Inside(rooms[i], p)) return i;
        return -1;
    }

    struct Plan {
        bool ok = false;
        int floor = -1;
        Path path;                 // main route: floor entry → (levers →) stairs down / exit volume (route.hpp)
        bool partial = false;      // the navmesh stopped short somewhere; the route jumps on to the next room
        std::vector<Box> rooms;    // the floor's room chain, for replanning the connector on a room change
        Marks marks;
        std::string why;           // log line: what was found or why there is no plan
    };

    // Game thread, dungeon only. pawn decides the floor; the route starts at its entry. O(rooms + triggers of the dungeon)
    // + O(rooms of the floor + levers) navmesh queries (one when nothing cuts the way).
    Plan Compute(V3 pawn);
    // Game thread: the connector, navmesh path from → to, joined straight to `to` where the navmesh stops. One query.
    Path Connect(V3 from, V3 to);
}

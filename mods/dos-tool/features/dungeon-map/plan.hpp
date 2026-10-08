#pragma once
// The current floor's main path for the local player (plan.cpp reads the dungeon and asks the game's navmesh).
// SDK-free types and choices; facts: references/game-facts.md § Dungeon.
#include <string>
#include <vector>
#include "scene.hpp"

namespace dungeon_map {
    constexpr float kDoorNear = 1500;  // a partial path's end lies this close to the door that stopped it

    struct Trigger {
        V3 at;
        int room = -1;  // index into Plan::rooms, -1 = none
        bool door = false, lever = false, closed = false, locked = false;
    };

    // The closed door nearest the end of a partial path (the navmesh stops in front of it), -1 = none within kDoorNear.
    inline int StoppingDoor(const std::vector<Trigger>& ts, V3 end) {
        int best = -1;
        float bd = kDoorNear;
        for (int i = 0; i < int(ts.size()); i++)
            if (ts[i].door && ts[i].closed)
                if (const float d = Dist(ts[i].at, end); d <= bd) { bd = d; best = i; }
        return best;
    }

    // Doors near the end of a partial path, for the diagnostic log (#83): counts and nearest distances, -1 = none.
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
        int seed = 0, floor = -1;  // key of the frontier
        Path path;                 // floor entry → stairs down / exit volume; ends early at a closed door
        bool partial = false;
        Marks marks;
        std::string why;           // log line: what was found or why there is no plan
    };

    // Game thread, dungeon only. pawn decides the floor. O(rooms + triggers of the dungeon) + one navmesh query.
    Plan Compute(V3 pawn);
}

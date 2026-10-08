#pragma once
// The floor's main route, kept current for every dungeon feature (dungeon-map, lever-markers): one plan, made on the game
// thread on game events (door state, lock, floor activation, entering a dungeon), shared. plan.hpp makes it.
#include <vector>
#include "plan.hpp"

namespace dungeon_map::planner {
    enum User : unsigned { kMap = 1, kLevers = 2 };
    // Any thread. The planner listens to game events while any user is on; with none it costs nothing.
    void Use(User u, bool on);
    bool Using(User u);  // any thread: is that user on (e.g. dungeon-map asks whether Lever markers owns the lever visuals)

    // Game thread (a listener's world tick). Updated on the planner's own world tick.
    bool InDungeon();
    const Plan& Current();  // ok = false: no plan (not in a dungeon, still loading)
    int Version();          // changes with every new plan
    bool Pawn(V3& out);     // the local player this tick, false = none

    // Render thread: the local player and the levers on the main route (locked door ahead), plain copies.
    void Levers(V3& pawn, std::vector<V3>& at);
}

#pragma once

// What the hub features need to know about each other (render thread only; features don't include each other).
namespace hub_state {
    inline bool walking = false;     // tavern hub drives the hero (F7)
    inline bool screenOpen = false;  // a game screen is open (inventory, NPC menu, map): the game has the input
    inline float camYaw = 0;         // the follow camera's yaw (degrees)
    inline bool build = false;       // build mode is on: the tavern hub stops aiming at NPCs
    inline bool carrying = false;    // build mode carries something: Page Up/Down are its, not the room floor's
}

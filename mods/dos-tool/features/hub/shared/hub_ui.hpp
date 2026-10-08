#pragma once
#include <cstdint>

// The hub's own click targets (BP_TriggerVolumeButton_*) while you walk the hub: buildings, world map and bank are
// hidden with their indicators; NPC click zones stay (they move with their NPC) and are driven like a gamepad does:
// focus (name shows) via I_HubTriggerSetGamepadFocused, select (menu opens) via I_HubTriggerGamepadSelect. The
// world map click zones (Button_Map_*, on the miniature map) stay and work the same way.
// Game thread work, queued from the render thread.
namespace hub_ui {
    void SetButtonsHidden(bool hidden);   // hidden: every non-NPC hub button off; false: back as the hub had them
    void Focus(bool on, float npcX, float npcY, float npcZ);  // the NPC click zone nearest that spot; off = unfocus it
    void Talk(float npcX, float npcY, float npcZ);  // the NPC click zone nearest to that spot (within 500), pressed
    void Stop();                          // render thread: listener off (may wait for in-flight game calls), once
}

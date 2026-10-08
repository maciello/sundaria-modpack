#pragma once
#include <cstdint>

// The hub's own click targets (BP_TriggerVolumeButton_*: buildings, NPCs, world map, bank) while you walk the hub:
// hidden with their indicators and made unclickable, and an NPC's menu opened by pressing its click zone the way
// the gamepad does (I_HubTriggerGamepadSelect). Game thread work, queued from the render thread.
namespace hub_ui {
    void SetButtonsHidden(bool hidden);   // hidden: every hub button off; false: back as the hub had them
    void Talk(float npcX, float npcY, float npcZ);  // the NPC click zone nearest to that spot (within 500), pressed
    void Stop();                          // render thread: listener off (may wait for in-flight game calls), once
}

// just test
#include "menu_groups.hpp"
#include <cassert>
#include <cstdio>

using namespace menu_groups;

int main() {
    std::string top, sub;
    Folders("C:/x/mods/dos-tool/features/targeting/cast-mode/cast-mode.cpp", top, sub);
    assert(top == "targeting" && sub == "cast-mode");
    Folders("C:\\x\\mods\\dos-tool\\features\\camera\\camera.cpp", top, sub);  // backslashes, no domain
    assert(top == "camera" && sub.empty());
    Folders("features/hub/quiet-smith/quiet-smith.cpp", top, sub);  // relative source path
    assert(top == "hub" && sub == "quiet-smith");
    Folders("C:/x/mods/dos-tool/core/overlay.cpp", top, sub);
    assert(top.empty());

    const char* p = "/r/mods/dos-tool/features/";
    auto of = [&](const char* rest) { return Of(std::string(p) + rest); };
    assert(of("targeting/cast-mode/cast-mode.cpp") == Group::Abilities);
    assert(of("targeting/training-ally/training-ally.cpp") == Group::Abilities);
    assert(of("game-feel/roll-cancel/roll-cancel.cpp") == Group::Movement);
    assert(of("air-control/air-control.cpp") == Group::Movement);
    assert(of("damage-numbers/damage-numbers.cpp") == Group::Combat);
    assert(of("inventory/item-upgrade/item-upgrade.cpp") == Group::Loot);
    assert(of("hub/tavern-hub/tavern-hub.cpp") == Group::World);
    assert(of("free-camera/free-camera.cpp") == Group::CameraGraphics);
    // probes and dev tools win over their domain
    assert(of("hub/mesh-probe/mesh-probe.cpp") == Group::Dev);
    assert(of("inventory/dps-probe/dps-probe.cpp") == Group::Dev);
    assert(of("inventory/char-snapshot/char-snapshot.cpp") == Group::Dev);
    assert(of("skeleton-probe/skeleton-probe.cpp") == Group::Dev);
    assert(of("live-bridge/live-bridge.cpp") == Group::Dev);
    assert(of("some-new-thing/x.cpp") == Group::Other);
    assert(Of("") == Group::Other);
    assert(std::string(Name(Group::Movement)) == "Movement & feel" && std::string(Name(Group::Other)) == "Other");
    std::puts("ok");
}

#pragma once
// DPS scores of the owned items (bag, bank) for every saved hero (#117), from libs/dps. Game thread only.
#include "item-upgrade.hpp"
#include "../shared/tiles.hpp"
#include <string>

namespace item_upgrade::scores {
    // Re-reads what changed: owned items (raw-record hash), the current hero and its equipped set (scores are dropped
    // then), the other heroes' snapshots (on a hero switch). false = no DPS data yet (why: Status()).
    // changed = verdicts may differ from the last call: re-evaluate every slot.
    bool Refresh(bool& changed);
    Verdict For(const items::tiles::Pos& p);  // cached per item key until the hero or its equipped set changes
    const std::string& Status();
    void Reset();  // Off(): drop everything
}

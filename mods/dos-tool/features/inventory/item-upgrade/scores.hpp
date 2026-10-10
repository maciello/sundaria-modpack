#pragma once
// DPS scores of the owned items (bag, bank) for every saved hero (#117), from libs/dps. Game thread only.
#include "item-upgrade.hpp"
#include "../shared/tiles.hpp"
#include <string>

namespace item_upgrade::scores {
    // Re-reads what changed: owned items (raw-record hash), the current hero and its equipped set (scores are dropped
    // then), the other heroes' snapshots (on a hero switch). false = no DPS data yet (why: Status()).
    // changed = verdicts may differ from the last call: re-evaluate every slot. The held weapon set (controller WeaponMode) is part of what is compared.
    bool Refresh(bool& changed);
    void Invalidate();  // the equipped set changed: the next Refresh re-reads and re-scores
    Verdict For(const items::tiles::Pos& p);  // cached per item key until the hero or its equipped set changes
    const std::string& Status();
    bool Preview();  // dev install without DPS tables: fake values to check the look; the details say so
    void Reset();  // Off(): drop everything
}

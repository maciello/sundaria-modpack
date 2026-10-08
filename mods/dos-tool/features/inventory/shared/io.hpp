#pragma once
#include "model.hpp"
#include <cstdint>

// Game reads (names.cpp, spec.cpp, read.cpp, probe.cpp). Handles are SDK pointers as void* (core/game.hpp convention).
namespace items::io {
    struct Names {
        std::vector<std::string> container, weaponType, equipSlot;  // enum value → name
        std::vector<std::string> stat;                              // Stat::type → attribute name
    };
    void Tick();                 // render thread, any rate (self-throttled to 1 Hz): enum/attribute names until loaded
    bool Ready();                // names loaded
    const Names& GetNames();     // valid once Ready()

    // Game thread from here on (stats come from a UFunction).
    struct Located {
        void* pc = nullptr;      // ABP_PlayerControllerOnline_C
        void* inv = nullptr;     // UBP_InvManagerComponent_C
        void* bag = nullptr;     // UBP_ItemContainerComponent_C: bag (type 0) + equipped (type 1) + temp loot
        void* bank = nullptr;    // UBP_ItemContainerStorage_C (pc.ItemContainerStorage; empty until the bank is opened)
        std::string how;
    };
    Located Locate();
    std::vector<Item> Read(void* container, bool bank, bool stats);  // every container type in it
    // Bank items: live when the game has them loaded (bank open), else the last live read of this session.
    struct Bank { std::vector<Item> items; bool live = false, seen = false; };
    Bank ReadBank(bool stats);
    std::string ContainersReport();
    bool CanSalvage(int specId);           // the spec's own I_CanSalvage, cached per spec id
    std::uint64_t Signature(void* container);  // hash of the container's raw item records: changes when items change  // every live item container (class, owner, item count): debug
}
